#include "project_composition_resolver.hpp"

#include "project_configuration_loader.hpp"
#include "../diagnostics/diagnostic_descriptor.hpp"
#include "../diagnostics/diagnostic_registry.hpp"
#include "../metrics/scoped_timer.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cw::server {
namespace {

// Holds one deduplicated project configuration while the parallel load phase
// resolves the complete composition graph.
struct cached_project {
    project_configuration configuration;
    bool loaded = false;
};

// Preserves the first project-load failure across worker threads without moving
// thread-local diagnostic storage into the shared resolver state.
struct failure_summary {
    status_code code = status_code::configuration_failed;
    diagnostic_id id = diagnostics::project_composition_failed.id;
    std::uint32_t offset = 0;
};

// Emits diagnostics on a best-effort basis so reporting cannot replace the
// original composition failure with an allocation failure.
bool try_emit(
    diagnostic_buffer& buffer,
    const diagnostic_descriptor& descriptor,
    operation_id operation,
    std::uint32_t offset = 0) noexcept {

    try {
        buffer.emit({
            descriptor.id,
            descriptor.default_severity,
            operation,
            {{}, offset, 0},
            {}
        });

        return true;
    }
    catch (const std::bad_alloc&) {
        return false;
    }
    catch (const std::length_error&) {
        return false;
    }
}

std::filesystem::path normalized_absolute(
    const std::filesystem::path& value,
    std::error_code& error) {

    auto result = std::filesystem::absolute(value, error);
    return error
               ? std::filesystem::path{}
               : result.lexically_normal();
}

} // namespace

status resolve_project_composition(
    const std::filesystem::path& root_configuration_path,
    const project_configuration& root_configuration,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    project_root_sink& roots,
    project_composition_statistics* statistics) noexcept {

    metrics.increment(metric_id::project_composition_resolve_count);

    scoped_timer total_timer{
        metrics,
        metric_id::project_composition_resolve_duration
    };

    try {
        std::error_code path_error;
        const auto root_path =
            normalized_absolute(root_configuration_path, path_error);

        if (path_error) {
            try_emit(
                diagnostics,
                diagnostics::project_composition_failed,
                operation);

            return {status_code::configuration_failed};
        }

        using node_ptr = std::shared_ptr<cached_project>;

        std::unordered_map<std::filesystem::path, node_ptr> cache;
        std::deque<std::filesystem::path> queue;
        std::mutex mutex;
        std::condition_variable ready;
        std::atomic_bool internal_failure = false;

        std::size_t outstanding = 0;
        std::size_t active_workers = 0;
        std::size_t cache_hits = 0;
        bool stop = false;

        std::optional<failure_summary> first_failure;
        project_composition_statistics measured;

        measured.worker_limit =
            (std::min)(
                std::size_t{8},
                (std::max)(
                    std::size_t{1},
                    static_cast<std::size_t>(
                        std::thread::hardware_concurrency())));

        auto root_node = std::make_shared<cached_project>();
        root_node->configuration = root_configuration;
        root_node->loaded = true;
        cache.emplace(root_path, std::move(root_node));

        // Under mutex, every non-root cache entry is queued, active, loaded, or
        // failed. outstanding is exactly the number of queued plus active jobs.
        const auto schedule_locked =
            [&](const std::filesystem::path& path) {

                if (cache.find(path) != cache.end()) {
                    ++cache_hits;
                    return;
                }

                auto node = std::make_shared<cached_project>();
                const auto [position, inserted] =
                    cache.emplace(path, std::move(node));

                if (!inserted) {
                    return;
                }

                try {
                    queue.push_back(path);
                }
                catch (const std::bad_alloc&) {
                    cache.erase(position);
                    throw;
                }
                catch (const std::length_error&) {
                    cache.erase(position);
                    throw;
                }

                ++outstanding;
            };

        for (const auto& item : root_configuration.project) {
            if (item.role == project_item_role::project) {
                schedule_locked(item.path);
            }
        }

        stop = outstanding == 0;

        const auto worker_body = [&] {
            while (true) {
                std::filesystem::path path;
                node_ptr node;

                {
                    std::unique_lock lock{mutex};

                    ready.wait(
                        lock,
                        [&] {
                            return internal_failure.load() ||
                                   stop ||
                                   !queue.empty();
                        });

                    if (internal_failure.load() || queue.empty()) {
                        return;
                    }

                    path = std::move(queue.front());
                    queue.pop_front();

                    node = cache.find(path)->second;
                    ++active_workers;

                    measured.max_active_workers =
                        (std::max)(
                            measured.max_active_workers,
                            active_workers);
                }

                project_configuration loaded;
                diagnostic_buffer local_diagnostics;

                const auto result =
                    load_project_configuration_file(
                        path,
                        operation,
                        local_diagnostics,
                        metrics,
                        loaded);

                // Every successfully dequeued job reaches this single completion
                // point and decrements outstanding exactly once.
                std::unique_lock lock{mutex};

                --active_workers;
                ++measured.configuration_files_loaded;

                if (!result.ok()) {
                    if (!first_failure) {
                        failure_summary summary;
                        summary.code = result.code;

                        if (!local_diagnostics.empty()) {
                            const auto& record =
                                local_diagnostics.records()[0];

                            summary.id = record.id;
                            summary.offset = record.location.offset;
                        }

                        first_failure = summary;
                    }
                }
                else {
                    node->configuration = std::move(loaded);
                    node->loaded = true;

                    for (const auto& item : node->configuration.project) {
                        if (item.role == project_item_role::project) {
                            schedule_locked(item.path);
                        }
                    }
                }

                if (--outstanding == 0) {
                    stop = true;
                }

                ready.notify_all();
            }
        };

        // No exception escapes a worker entry point. Exceptional termination
        // wakes all waiters and supersedes normal outstanding accounting.
        const auto worker = [&] {
            try {
                worker_body();
            }
            catch (const std::bad_alloc&) {
                internal_failure.store(true);
                ready.notify_all();
            }
            catch (const std::length_error&) {
                internal_failure.store(true);
                ready.notify_all();
            }
            catch (const std::system_error&) {
                internal_failure.store(true);
                ready.notify_all();
            }
        };

        if (outstanding != 0) {
            std::vector<std::jthread> workers;
            workers.reserve(measured.worker_limit);

            for (std::size_t index = 0;
                 index < measured.worker_limit;
                 ++index) {
                workers.emplace_back(worker);
            }

            for (auto& thread : workers) {
                thread.join();
            }
        }

        if (statistics) {
            *statistics = measured;
        }

        metrics.increment(
            metric_id::project_composition_file_count,
            static_cast<std::uint64_t>(
                measured.configuration_files_loaded));

        metrics.increment(
            metric_id::project_composition_cache_hit_count,
            static_cast<std::uint64_t>(cache_hits));

        metrics.set(
            metric_id::project_composition_max_parallel_workers,
            static_cast<std::int64_t>(
                measured.max_active_workers));

        if (internal_failure.load()) {
            try_emit(
                diagnostics,
                diagnostics::project_initialization_failed,
                operation);

            return {status_code::initialization_failed};
        }

        if (first_failure) {
            const auto* descriptor =
                diagnostic_registry.find(first_failure->id);

            try_emit(
                diagnostics,
                descriptor
                    ? *descriptor
                    : diagnostics::project_composition_failed,
                operation,
                first_failure->offset);

            return {first_failure->code};
        }

        enum class visit_state : std::uint8_t {
            unseen,
            visiting,
            done
        };

        struct visit_frame {
            std::filesystem::path path;
            const project_configuration* configuration = nullptr;
            std::size_t next_item = 0;
        };

        std::unordered_map<std::filesystem::path, visit_state> states;
        std::vector<visit_frame> stack;

        const auto root = cache.find(root_path);

        if (root == cache.end() || !root->second->loaded) {
            try_emit(
                diagnostics,
                diagnostics::project_composition_failed,
                operation);

            return {status_code::configuration_failed};
        }

        states.emplace(root_path, visit_state::visiting);
        stack.push_back({
            root_path,
            &root->second->configuration,
            0
        });

        // Traversal is iterative so composition depth does not consume the
        // process call stack. visiting marks detect project-reference cycles.
        while (!stack.empty()) {
            auto& frame = stack.back();

            if (frame.next_item ==
                frame.configuration->project.size()) {
                states.find(frame.path)->second = visit_state::done;
                stack.pop_back();
                continue;
            }

            const auto& item =
                frame.configuration->project[frame.next_item++];

            if (item.role != project_item_role::project) {
                const auto published =
                    roots.add(item.path, item.role);

                if (!published.ok()) {
                    try_emit(
                        diagnostics,
                        published.code ==
                                status_code::initialization_failed
                            ? diagnostics::project_initialization_failed
                            : diagnostics::project_composition_failed,
                        operation);

                    return published;
                }

                continue;
            }

            const auto state_position =
                states.try_emplace(
                    item.path,
                    visit_state::unseen).first;

            if (state_position->second == visit_state::visiting) {
                try_emit(
                    diagnostics,
                    diagnostics::project_composition_cycle,
                    operation);

                return {status_code::configuration_failed};
            }

            if (state_position->second == visit_state::done) {
                continue;
            }

            const auto child = cache.find(item.path);

            if (child == cache.end() || !child->second->loaded) {
                try_emit(
                    diagnostics,
                    diagnostics::project_composition_failed,
                    operation);

                return {status_code::configuration_failed};
            }

            state_position->second = visit_state::visiting;

            stack.push_back({
                item.path,
                &child->second->configuration,
                0
            });
        }

        return {};
    }
    catch (const std::bad_alloc&) {
        try_emit(
            diagnostics,
            diagnostics::project_initialization_failed,
            operation);

        return {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        try_emit(
            diagnostics,
            diagnostics::project_initialization_failed,
            operation);

        return {status_code::initialization_failed};
    }
    catch (const std::filesystem::filesystem_error&) {
        try_emit(
            diagnostics,
            diagnostics::project_composition_failed,
            operation);

        return {status_code::configuration_failed};
    }
    catch (const std::system_error&) {
        try_emit(
            diagnostics,
            diagnostics::project_initialization_failed,
            operation);

        return {status_code::initialization_failed};
    }
}

} // namespace cw::server
