#include "source_frontend_generation.hpp"

#include "source_frontend_cache.hpp"
#include "source_publisher.hpp"
#include "../builder/project_builder.hpp"
#include "../graph/graph_build_transaction.hpp"
#include "../parser/parser.hpp"
#include "../parser/source_context.hpp"
#include "../source/source_manager.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"
#include "../../metrics/source_acquisition_telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace cw::server {
namespace {

status emit_acquisition_failure(
    diagnostic_buffer& diagnostics,
    operation_id operation,
    source_id source,
    status result) noexcept {

    try {
        diagnostics.emit({
            diagnostics::source_acquisition_failed.id,
            diagnostics::source_acquisition_failed.default_severity,
            operation,
            {source, 0, 0},
            {}
        });
    }
    catch (...) {
        return {status_code::initialization_failed};
    }

    return result;
}

} // namespace

source_frontend_generation::source_frontend_generation(
    graph_build_transaction& build_transaction,
    source_frontend_cache& frontend_cache,
    language_configuration language_config) noexcept
    : transaction(&build_transaction),
      backend(&default_parser_backend()),
      cache(&frontend_cache),
      language(language_config) {}


status source_frontend_generation::populate_cache(
    source_frontend_cache& destination) noexcept {

    try {
        auto update = destination.begin_update(true);

        {
            std::lock_guard lock{mutex};

            if (!failure.ok()) {
                return failure;
            }

            for (std::size_t index = 0;
                 index < states.size();
                 ++index) {
                auto& state = states[index];

                if (!state.interface) {
                    continue;
                }

                const auto result = update.replace(
                    source_id{
                        static_cast<std::uint32_t>(index + 1)
                    },
                    std::move(state.interface));

                if (!result.ok()) {
                    return result;
                }
            }
        }

        auto result = update.prepare_publish(states.size());

        if (!result.ok()) {
            return result;
        }

        update.publish_prepared();
        return {};
    }
    catch (...) {
        destination.invalidate();
        return {status_code::initialization_failed};
    }
}

source_rebuild_result source_frontend_generation::rebuild_incremental(
    operation_id operation,
    diagnostic_buffer& diagnostics,
    source_acquisition_telemetry& telemetry,
    const project_builder& builder,
    std::span<const source_id> dirty_sources,
    bool reconcile_all) noexcept {

    if (transaction == nullptr ||
        cache == nullptr ||
        !cache->complete()) {
        return status{status_code::not_available};
    }

    try {
        const auto initial_source_count =
            transaction->sources().source_count();

        current_summary = {};
        current_summary.reconciliation = reconcile_all;

        std::vector<std::uint8_t> selected(
            initial_source_count + 1,
            0);

        std::vector<source_id> observation_sources;

        if (reconcile_all) {
            observation_sources.reserve(
                initial_source_count);

            for (std::uint32_t value = 1;
                 value <= initial_source_count;
                 ++value) {
                observation_sources.push_back(
                    source_id{value});
            }
        }
        else {
            observation_sources.reserve(
                dirty_sources.size());

            for (const auto source : dirty_sources) {
                if (!source ||
                    source.value() >
                        initial_source_count) {
                    return status{
                        status_code::invalid_state
                    };
                }

                if (selected[source.value()] != 0) {
                    continue;
                }

                selected[source.value()] = 1;
                observation_sources.push_back(source);
            }

            std::sort(
                observation_sources.begin(),
                observation_sources.end(),
                [](source_id left, source_id right) noexcept {
                    return left.value() < right.value();
                });
        }

        current_summary.dirty =
            static_cast<std::uint32_t>(
                reconcile_all
                    ? dirty_sources.size()
                    : observation_sources.size());

        std::vector<std::uint8_t> checked(
            initial_source_count + 1,
            0);

        struct observation_slot {
            source_id source{};
            source_acquire_job job;
            source_acquire_result acquired;
            status result{};
        };

        std::vector<observation_slot> observations(
            observation_sources.size());

        for (std::size_t index = 0;
             index < observation_sources.size();
             ++index) {
            auto& slot = observations[index];
            slot.source = observation_sources[index];

            slot.result =
                transaction->sources().prepare_acquire(
                    slot.source,
                    slot.job);

            if (!slot.result.ok()) {
                auto result = emit_acquisition_failure(
                    diagnostics,
                    operation,
                    slot.source,
                    slot.result);

                std::lock_guard lock{mutex};
                fail_locked(result);
                return result;
            }
        }

        const auto hardware_threads =
            std::thread::hardware_concurrency();

        const auto available_observation_workers =
            (std::min)(
                std::size_t{32},
                static_cast<std::size_t>(
                    hardware_threads == 0
                        ? 1
                        : hardware_threads));

        const auto worker_count =
            observations.empty()
                ? std::size_t{0}
                : (std::min)(
                      available_observation_workers,
                      observations.size());

        std::vector<source_acquisition_telemetry>
            local_telemetry;

        local_telemetry.reserve(worker_count);

        for (std::size_t index = 0;
             index < worker_count;
             ++index) {
            local_telemetry.emplace_back(
                telemetry.mode());
        }

        std::atomic_size_t next_index{0};
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);

        for (std::size_t worker_index = 0;
             worker_index < worker_count;
             ++worker_index) {
            workers.emplace_back(
                [&, worker_index]() {
                    for (;;) {
                        const auto index =
                            next_index.fetch_add(
                                1,
                                std::memory_order_relaxed);

                        if (index >= observations.size()) {
                            break;
                        }

                        auto& slot =
                            observations[index];

                        slot.result =
                            source_manager_update::
                                execute_acquire(
                                    slot.job,
                                    local_telemetry[
                                        worker_index],
                                    slot.acquired);
                    }
                });
        }

        workers.clear();

        for (const auto& local :
             local_telemetry) {
            telemetry.merge_from(local);
        }

        for (auto& slot : observations) {
            if (!slot.result.ok()) {
                auto result = emit_acquisition_failure(
                    diagnostics,
                    operation,
                    slot.source,
                    slot.result);

                std::lock_guard lock{mutex};
                fail_locked(result);
                return result;
            }

            auto result =
                transaction->sources().apply_acquire(
                    slot.job,
                    std::move(slot.acquired),
                    telemetry);

            if (!result.ok()) {
                result = emit_acquisition_failure(
                    diagnostics,
                    operation,
                    slot.source,
                    result);

                std::lock_guard lock{mutex};
                fail_locked(result);
                return result;
            }

            checked[slot.source.value()] = 1;
        }

        current_summary.checked =
            static_cast<std::uint32_t>(
                observations.size());

        std::vector<std::uint8_t> affected(
            initial_source_count + 1,
            0);

        std::vector<source_id> affected_sources;
        std::vector<source_id> dependents;

        const auto mark_affected =
            [&](source_id source) {
                if (!source) {
                    return;
                }

                if (affected.size() <= source.value()) {
                    affected.resize(
                        static_cast<std::size_t>(
                            source.value()) + 1,
                        0);
                }

                if (affected[source.value()] == 0) {
                    affected[source.value()] = 1;
                    affected_sources.push_back(source);
                }
            };

        for (const auto& change :
             transaction->sources().changes()) {
            mark_affected(change.source);

            const auto dependent_result =
                transaction->sources().collect_dependents(
                    change.source,
                    dependents);

            if (!dependent_result.ok()) {
                std::lock_guard lock{mutex};
                fail_locked(dependent_result);
                return dependent_result;
            }

            for (const auto dependent : dependents) {
                mark_affected(dependent);
            }
        }

        current_summary.affected =
            static_cast<std::uint32_t>(
                affected_sources.size());

        for (const auto source : affected_sources) {
            source_physical_state physical;

            auto result =
                transaction->sources().get_physical_state(
                    source,
                    physical);

            if (!result.ok()) {
                std::lock_guard lock{mutex};
                fail_locked(result);
                return result;
            }

            if (physical.presence ==
                source_presence::missing) {
                result = transaction->sources().set_includes(
                    source,
                    {});

                if (!result.ok()) {
                    std::lock_guard lock{mutex};
                    fail_locked(result);
                    return result;
                }

                std::lock_guard lock{mutex};
                auto& state = ensure(source);

                state.removed = true;
                state.parsed = true;
                state.build_entry =
                    std::make_unique<source_build_entry>();

                state.build_entry->source = source;
                continue;
            }

            result = enqueue(source);

            if (!result.ok()) {
                std::lock_guard lock{mutex};
                fail_locked(result);
                return result;
            }
        }

        for (;;) {
            source_id source;

            if (!take_discovery(source)) {
                break;
            }

            if (checked.size() <= source.value()) {
                checked.resize(
                    static_cast<std::size_t>(
                        source.value()) + 1,
                    0);
            }

            if (checked[source.value()] == 0) {
                auto result =
                    transaction->sources().acquire(
                        source,
                        telemetry);

                ++current_summary.checked;

                if (!result.ok()) {
                    result = emit_acquisition_failure(
                        diagnostics,
                        operation,
                        source,
                        result);

                    std::lock_guard lock{mutex};
                    fail_locked(result);
                    return result;
                }

                checked[source.value()] = 1;

                if (affected.size() <= source.value()) {
                    affected.resize(
                        static_cast<std::size_t>(
                            source.value()) + 1,
                        0);
                }

                if (affected[source.value()] == 0) {
                    affected[source.value()] = 1;
                    ++current_summary.affected;
                }
            }

            const auto result = discover(
                source,
                operation,
                diagnostics);

            if (!result.ok()) {
                return result;
            }
        }

        auto result = finish_discovery(
            operation,
            diagnostics);

        if (!result.ok()) {
            return result;
        }

        source_context context;

        for (;;) {
            source_id source;

            if (!take_semantic_ready(source)) {
                break;
            }

            result = parse_and_capture(
                source,
                operation,
                context);

            try {
                diagnostics.reserve(
                    diagnostics.records().size() +
                    context.diagnostics.records().size());

                for (const auto& record :
                     context.diagnostics.records()) {
                    diagnostics.emit(record);
                }
            }
            catch (...) {
                result = {
                    status_code::initialization_failed
                };
            }

            context.reset();

            if (!result.ok()) {
                std::lock_guard lock{mutex};
                fail_locked(result);
                return result;
            }
        }

        {
            std::lock_guard lock{mutex};

            if (!failure.ok()) {
                return failure;
            }

            for (const auto& state : states) {
                if (state.discovery_claimed &&
                    state.discovery_done &&
                    !state.parsed) {
                    const status incomplete{
                        status_code::invalid_state
                    };

                    fail_locked(incomplete);
                    return incomplete;
                }
            }
        }

        for (std::size_t index = 0;
             index < states.size();
             ++index) {
            auto& state = states[index];

            if (!state.build_entry ||
                state.published) {
                continue;
            }

            result = publish_source_entry(
                *transaction,
                *state.build_entry,
                builder,
                operation,
                diagnostics);

            if (!result.ok()) {
                return result;
            }

            state.published = true;
            ++state.counts.publish;
        }

        diagnostics.sort_deterministic();

        auto cache_update = cache->begin_update(false);

        for (std::size_t index = 0;
             index < states.size();
             ++index) {
            auto& state = states[index];

            if (!state.build_entry) {
                continue;
            }

            result = cache_update.replace(
                source_id{
                    static_cast<std::uint32_t>(index + 1)
                },
                state.removed
                    ? std::unique_ptr<
                          source_environment_storage>{}
                    : std::move(state.interface));

            if (!result.ok()) {
                return result;
            }
        }

        result = cache_update.prepare_publish(
            transaction->sources().source_count());

        if (!result.ok()) {
            return result;
        }

        result = transaction->commit();

        if (!result.ok()) {
            cache_update.cancel();
            return result;
        }

        cache_update.publish_prepared();

        source_rebuild_result completed;
        completed.semantic = result;
        completed.frontend = summary();
        return completed;
    }
    catch (...) {
        const status result{
            status_code::initialization_failed
        };

        std::lock_guard lock{mutex};
        fail_locked(result);
        return result;
    }
}

} // namespace cw::server
