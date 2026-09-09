#include "source_frontend_generation.hpp"

#include "source_frontend_cache.hpp"
#include "source_publisher.hpp"
#include "../builder/project_builder.hpp"
#include "../builder/source_build_entry.hpp"
#include "../graph/graph_build_transaction.hpp"
#include "../parser/parser.hpp"
#include "../parser/source_context.hpp"
#include "../parser/source_environment.hpp"
#include "../source/source_manager.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"
#include "../../metrics/source_acquisition_telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <limits>
#include <thread>

namespace cw::server {
namespace {



std::uint64_t g0_elapsed_ns(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - begin).count());
}

} // namespace

source_frontend_generation::source_frontend_generation(
    graph_build_transaction& build_transaction,
    language_configuration language_config) noexcept
    : transaction(&build_transaction),
      backend(&default_parser_backend()),
      language(language_config) {}

source_frontend_generation::source_frontend_generation(
    graph_build_transaction& build_transaction,
    const parser_backend& parser,
    language_configuration language_config) noexcept
    : transaction(&build_transaction),
      backend(&parser),
      language(language_config) {}

source_frontend_generation::source_state&
source_frontend_generation::ensure(source_id source) {
    if (cache != nullptr) {
        return sparse_states.try_emplace(
            source.value()).first->second;
    }

    if (states.size() < source.value()) {
        states.resize(source.value());
    }

    return states[source.value() - 1];
}

source_frontend_generation::source_state*
source_frontend_generation::find_state(
    source_id source) noexcept {

    if (!source) {
        return nullptr;
    }

    if (cache != nullptr) {
        const auto found =
            sparse_states.find(source.value());

        return found == sparse_states.end()
            ? nullptr
            : &found->second;
    }

    return source.value() <= states.size()
        ? &states[source.value() - 1]
        : nullptr;
}

const source_frontend_generation::source_state*
source_frontend_generation::find_state(
    source_id source) const noexcept {

    if (!source) {
        return nullptr;
    }

    if (cache != nullptr) {
        const auto found =
            sparse_states.find(source.value());

        return found == sparse_states.end()
            ? nullptr
            : &found->second;
    }

    return source.value() <= states.size()
        ? &states[source.value() - 1]
        : nullptr;
}

void source_frontend_generation::fail_locked(
    status result) noexcept {

    if (failure.ok()) {
        failure = result;
    }

    discovery_queue.clear();
    semantic_queue.clear();
    discovery_condition.notify_all();
    semantic_condition.notify_all();

    transaction->fail(failure);
}

void source_frontend_generation::enqueue_ready_locked(
    source_id source,
    source_state& state) {

    if (failure.ok() &&
        state.discovery_done &&
        state.remaining == 0 &&
        !state.parse_claimed &&
        !state.semantic_queued &&
        !state.parsed) {
        state.semantic_queued = true;
        semantic_queue.push_back(source);
        semantic_condition.notify_one();
    }
}

status source_frontend_generation::enqueue(
    source_id source) noexcept {

    if (!source || !transaction) {
        return {
            status_code::invalid_state
        };
    }

    try {
        std::lock_guard lock{mutex};

        if (!failure.ok()) {
            return failure;
        }

        auto& state = ensure(source);

        if (!state.discovery_claimed) {
            state.discovery_claimed = true;
            discovery_queue.push_back(source);
            discovery_condition.notify_one();
        }

        return {};
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

bool source_frontend_generation::take_discovery(
    source_id& source) noexcept {

    source = {};

    std::lock_guard lock{mutex};

    if (!failure.ok() ||
        discovery_queue.empty()) {
        return false;
    }

    source = discovery_queue.front();
    discovery_queue.pop_front();

    ++active_discoveries;
    return true;
}

status source_frontend_generation::discover(
    source_id source,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    const auto fail =
        [&](status result) noexcept {
            std::lock_guard lock{mutex};

            if (active_discoveries != 0) {
                --active_discoveries;
            }

            fail_locked(result);
            return result;
        };

    source_view view;
    status result;

    {
        std::lock_guard lock{mutex};

        result =
            transaction->sources().get_view(
                source,
                view);
    }

    if (!result.ok()) {
        return fail(result);
    }

    bool implementation_only = false;
    bool missing_state = false;

    {
        std::lock_guard lock{mutex};
        const auto* state = find_state(source);
        missing_state = state == nullptr;

        if (!missing_state) {
            implementation_only = state->implementation_only;
        }
    }

    if (missing_state) {
        return fail({status_code::invalid_state});
    }

    if (project_role_routing && implementation_only) {
        result = transaction->sources().set_includes(
            source,
            std::span<const source_id>{});

        if (!result.ok()) {
            return fail(result);
        }

        std::lock_guard lock{mutex};
        auto* state = find_state(source);

        if (state == nullptr) {
            if (active_discoveries != 0) {
                --active_discoveries;
            }
            fail_locked({status_code::invalid_state});
            return {status_code::invalid_state};
        }

        if (active_discoveries != 0) {
            --active_discoveries;
        }

        state->discovery_done = true;
        state->parse_claimed = true;
        state->parsed = true;
        semantic_condition.notify_all();
        return {};
    }

    std::vector<parser_token> tokens;
    std::vector<directive_span> directives;
    diagnostic_buffer local_diagnostics;

    result =
        lex_source(
            view,
            operation,
            local_diagnostics,
            tokens,
            &directives);

    const auto merge_local_diagnostics =
        [&]() noexcept -> status {
            try {
                diagnostics.reserve(
                    diagnostics.records().size() +
                    local_diagnostics.records().size());

                for (const auto& record :
                     local_diagnostics.records()) {
                    diagnostics.emit(record);
                }

                return {};
            }
            catch (...) {
                return {
                    status_code::initialization_failed
                };
            }
        };

    const auto merged =
        merge_local_diagnostics();

    if (!merged.ok()) {
        return fail(merged);
    }

    if (!result.ok()) {
        return fail(result);
    }


    try {
        std::vector<parser_token> semantic_tokens;
        std::vector<std::pair<std::string_view, std::uint32_t>> include_names;
        std::vector<bool> namespace_braces;

        semantic_tokens.reserve(tokens.size());
        include_names.reserve(directives.size());

        std::size_t cursor = 0;
        std::uint32_t namespace_depth = 0;
        bool namespace_pending = false;

        // Tracks only enough brace structure to enforce the current include
        // placement rule while preprocessing directives are removed.
        const auto scan_scopes =
            [&](std::size_t begin, std::size_t end) {
                for (auto index = begin;
                     index < end;
                     ++index) {
                    const auto& token =
                        tokens[index];

                    if (token.kind ==
                        parser_token_kind::keyword_namespace) {
                        namespace_pending = true;
                    }
                    else if (
                        token.kind ==
                            parser_token_kind::punctuation &&
                        token.punctuation ==
                            parser_punctuation::left_brace) {
                        namespace_braces.push_back(
                            namespace_pending);

                        if (namespace_pending) {
                            ++namespace_depth;
                        }

                        namespace_pending = false;
                    }
                    else if (
                        token.kind ==
                            parser_token_kind::punctuation &&
                        token.punctuation ==
                            parser_punctuation::right_brace) {
                        if (!namespace_braces.empty()) {
                            if (namespace_braces.back()) {
                                --namespace_depth;
                            }

                            namespace_braces.pop_back();
                        }

                        namespace_pending = false;
                    }
                    else if (
                        token.kind ==
                            parser_token_kind::punctuation &&
                        token.punctuation ==
                            parser_punctuation::semicolon) {
                        namespace_pending = false;
                    }
                }
            };

        const auto emit_error =
            [&](const diagnostic_descriptor& descriptor,
                const parser_token& token) noexcept {
                try {
                    diagnostics.emit({
                        descriptor.id,
                        descriptor.default_severity,
                        operation,
                        {
                            source,
                            token.offset,
                            token.length
                        },
                        {}
                    });
                }
                catch (...) {
                    return fail({
                        status_code::initialization_failed
                    });
                }

                return fail({
                    status_code::configuration_failed
                });
            };

        for (const auto span : directives) {
            if (span.token_begin < cursor ||
                span.token_end > tokens.size() ||
                span.token_end - span.token_begin < 2) {
                return fail({
                    status_code::configuration_failed
                });
            }

            scan_scopes(
                cursor,
                span.token_begin);

            semantic_tokens.insert(
                semantic_tokens.end(),
                std::next(tokens.begin(),
                          static_cast<std::ptrdiff_t>(cursor)),
                std::next(tokens.begin(),
                          static_cast<std::ptrdiff_t>(span.token_begin)));

            const auto& marker =
                tokens[span.token_begin];

            const auto& directive =
                tokens[span.token_begin + 1];

            const auto directive_name =
                view.bytes.substr(
                    directive.offset,
                    directive.length);

            if (marker.punctuation !=
                    parser_punctuation::hash ||
                directive.kind !=
                    parser_token_kind::identifier ||
                directive_name != "include" ||
                !language.preprocessor.include) {
                return emit_error(
                    diagnostics::source_unsupported_directive,
                    directive);
            }

            if (span.token_end -
                    span.token_begin != 3) {
                return emit_error(
                    diagnostics::parser_invalid_source,
                    directive);
            }

            const auto& argument =
                tokens[span.token_begin + 2];

            if (argument.kind !=
                    parser_token_kind::string_literal ||
                argument.length < 2) {
                return emit_error(
                    diagnostics::parser_invalid_source,
                    argument);
            }

            if (namespace_depth != 0) {
                return emit_error(
                    diagnostics::source_include_inside_namespace,
                    marker);
            }

            include_names.push_back({
                view.bytes.substr(
                    argument.offset + 1,
                    argument.length - 2),
                marker.offset
            });

            cursor = span.token_end;
        }

        semantic_tokens.insert(
            semantic_tokens.end(),
            std::next(tokens.begin(),
                      static_cast<std::ptrdiff_t>(cursor)),
            tokens.end());

        std::vector<source_id> dependencies;
        dependencies.reserve(include_names.size());

        std::vector<include_visibility> visibility;
        visibility.reserve(include_names.size());

        // Resolve include paths outside the frontend orchestration lock. Source
        // Manager serializes only its short identity/candidate mutation while
        // filesystem existence checks can proceed independently across workers.
        for (const auto& include : include_names) {
            source_id dependency;

            result =
                transaction->sources().resolve_include(
                    source,
                    include.first,
                    dependency);

            if (!result.ok()) {
                try {
                    diagnostics.emit({
                        diagnostics::source_include_not_found.id,
                        diagnostics::source_include_not_found.default_severity,
                        operation,
                        {source, 0, 0},
                        {}
                    });
                }
                catch (...) {
                    return fail({
                        status_code::initialization_failed
                    });
                }

                return fail(result);
            }

            visibility.push_back({
                include.second,
                dependency
            });

            if (std::find(
                    dependencies.begin(),
                    dependencies.end(),
                    dependency) == dependencies.end()) {
                dependencies.push_back(dependency);
            }
        }

        result =
            transaction->sources().set_includes(
                source,
                dependencies);

        if (!result.ok()) {
            return fail(result);
        }

        std::lock_guard lock{mutex};

        if (!failure.ok()) {
            if (active_discoveries != 0) {
                --active_discoveries;
            }

            discovery_condition.notify_all();
            return failure;
        }

        auto* current_state =
            find_state(source);

        if (current_state == nullptr ||
            !current_state->discovery_claimed) {
            if (active_discoveries != 0) {
                --active_discoveries;
            }

            discovery_condition.notify_all();
            return {
                status_code::invalid_state
            };
        }

        if (current_state->discovery_done) {
            if (active_discoveries != 0) {
                --active_discoveries;
            }

            discovery_condition.notify_all();
            return {};
        }

        for (const auto dependency : dependencies) {
            // ensure(dependency) may insert into sparse_states and rehash the
            // unordered_map. Reacquire the owning Source state afterward so no
            // pointer/reference survives a possible rehash.
            auto& dependency_state =
                ensure(dependency);

            auto& source_state =
                ensure(source);

            // Incremental generations reuse an unchanged dependency interface
            // directly from the committed COLD cache. A dependency already
            // claimed for discovery belongs to the affected closure and must
            // produce a new candidate interface instead.
            if (!dependency_state.discovery_claimed &&
                !dependency_state.published &&
                cache != nullptr) {
                dependency_state.cached_interface =
                    cache->interface(dependency);

                if (dependency_state.cached_interface != nullptr) {
                    dependency_state.published = true;
                }
            }

            if (!dependency_state.published) {
                ++source_state.remaining;
            }

            dependency_state.dependents.push_back(
                source);

            if (!dependency_state.discovery_claimed &&
                !dependency_state.published) {
                dependency_state.discovery_claimed = true;
                discovery_queue.push_back(dependency);
                discovery_condition.notify_one();
            }
        }

        auto& state =
            ensure(source);

        state.dependencies =
            std::move(dependencies);

        state.visibility =
            std::move(visibility);

        state.tokens =
            std::move(semantic_tokens);

        state.discovery_done = true;

        ++state.counts.discovery;
        ++state.counts.lex;

        if (active_discoveries != 0) {
            --active_discoveries;
        }

        discovery_condition.notify_all();

        enqueue_ready_locked(
            source,
            state);

        return {};
    }
    catch (...) {
        return fail({
            status_code::initialization_failed
        });
    }
}

bool source_frontend_generation::take_semantic_ready(
    source_id& source) noexcept {

    source = {};

    std::lock_guard lock{mutex};

    if (!failure.ok() ||
        semantic_queue.empty()) {
        return false;
    }

    source = semantic_queue.front();
    semantic_queue.pop_front();

    auto* state =
        find_state(source);

    if (state == nullptr) {
        fail_locked({
            status_code::invalid_state
        });
        source = {};
        return false;
    }

    state->semantic_queued = false;

    if (state->parse_claimed ||
        state->parsed) {
        source = {};
        return false;
    }

    state->parse_claimed = true;
    return true;
}

status source_frontend_generation::parse_and_capture_independent_g0(
    source_id source,
    operation_id operation,
    source_context& context) noexcept {

    try {
        if (!source ||
            cache != nullptr ||
            transaction == nullptr ||
            source.value() > states.size()) {
            return {status_code::invalid_state};
        }

        auto& state =
            states[source.value() - 1];

        if (!state.parse_claimed ||
            state.parsed ||
            !state.discovery_done ||
            state.remaining != 0 ||
            !state.dependencies.empty() ||
            !state.visibility.empty() ||
            !state.dependents.empty()) {
            return {status_code::invalid_state};
        }

        // Discovery is complete before these workers start. No Source Manager
        // mutation occurs until all workers join, so concurrent get_view reads
        // are against immutable candidate physical storage.
        source_view view;

        auto result =
            transaction->sources().get_view(
                source,
                view);

        if (!result.ok()) {
            return result;
        }

        const source_environment environment;

        result =
            backend->parse(
                view,
                state.tokens,
                environment,
                language,
                operation,
                context);

        if (!result.ok()) {
            return result;
        }



        auto build_entry =
            std::make_unique<
                source_build_entry>();

        result =
            context.release_facts(
                source,
                *build_entry);

        if (!result.ok()) {
            return result;
        }

        // This source_state belongs exclusively to the current static worker
        // partition. No shared queue/dependency counter needs synchronization.


        state.build_entry =
            std::move(build_entry);

        state.parsed = true;
        ++state.counts.parse;
        return {};
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

status source_frontend_generation::parse_and_capture(
    source_id source,
    operation_id operation,
    source_context& context) noexcept {

    try {
        source_view view;
        status result;

        {
            std::lock_guard lock{mutex};
            result = transaction->sources().get_view(source, view);
        }

        if (!result.ok()) {
            std::lock_guard lock{mutex};
            fail_locked(result);
            return result;
        }

        std::span<const parser_token> tokens;
        std::vector<const source_environment_storage*> dependency_interfaces;
        std::vector<source_environment_import> visible_imports;
        bool export_required = false;

        {
            std::lock_guard lock{mutex};

            if (!failure.ok()) {
                return failure;
            }

            const auto* state =
                find_state(source);

            if (state == nullptr ||
                !state->parse_claimed ||
                state->parsed ||
                !state->discovery_done ||
                state->remaining != 0) {
                return {status_code::invalid_state};
            }

            tokens = state->tokens;
            export_required =
                !state->dependents.empty();

            if (export_required) {
                dependency_interfaces.reserve(
                    state->dependencies.size());
            }

            for (const auto dependency :
                 state->dependencies) {
                const auto* dependency_state =
                    find_state(dependency);

                if (dependency_state == nullptr) {
                    return {status_code::invalid_state};
                }

                const auto* dependency_interface =
                    dependency_state->interface
                        ? dependency_state->interface.get()
                        : dependency_state->cached_interface;

                if (!dependency_interface) {
                    return {status_code::invalid_state};
                }

                if (export_required) {
                    dependency_interfaces.push_back(
                        dependency_interface);
                }
            }

            visible_imports.reserve(
                state->visibility.size());

            for (const auto& item :
                 state->visibility) {
                const auto* dependency_state =
                    find_state(item.dependency);

                if (dependency_state == nullptr) {
                    return {status_code::invalid_state};
                }

                const auto* dependency_interface =
                    dependency_state->interface
                        ? dependency_state->interface.get()
                        : dependency_state->cached_interface;

                if (!dependency_interface) {
                    return {status_code::invalid_state};
                }

                visible_imports.push_back({
                    item.visible_from,
                    dependency_interface
                });
            }
        }

        const source_environment environment{visible_imports};

        result = backend->parse(
            view,
            tokens,
            environment,
            language,
            operation,
            context);

        if (!result.ok()) {
            std::lock_guard lock{mutex};
            fail_locked(result);
            return result;
        }

        std::unique_ptr<source_environment_storage>
            published_interface;

        if (export_required) {
            published_interface =
                std::make_unique<
                    source_environment_storage>();

            result =
                published_interface->initialize(
                    source,
                    context,
                    dependency_interfaces);

            if (!result.ok()) {
                std::lock_guard lock{mutex};
                fail_locked(result);
                return result;
            }
        }

        auto build_entry =
            std::make_unique<source_build_entry>();

        result =
            context.release_facts(
                source,
                *build_entry);

        if (!result.ok()) {
            std::lock_guard lock{mutex};
            fail_locked(result);
            return result;
        }

        // From this point the Parser context is no longer referenced by build
        // state. Only source_id-owned interface/contribution data is published.
        std::lock_guard lock{mutex};

        auto* state =
            find_state(source);

        if (state == nullptr) {
            const status invalid{
                status_code::invalid_state
            };

            fail_locked(invalid);
            return invalid;
        }

        state->interface = std::move(published_interface);
        state->build_entry = std::move(build_entry);
        state->parsed = true;
        ++state->counts.parse;

        if (semantic_scheduler_active &&
            semantic_remaining != 0) {
            --semantic_remaining;
        }

        for (const auto dependent_id :
             state->dependents) {
            auto* dependent =
                find_state(dependent_id);

            if (dependent == nullptr) {
                const status invalid{
                    status_code::invalid_state
                };

                fail_locked(invalid);
                return invalid;
            }

            if (dependent->remaining != 0) {
                --dependent->remaining;
            }

            enqueue_ready_locked(
                dependent_id,
                *dependent);
        }

        semantic_condition.notify_all();
        return {};
    }
    catch (...) {
        const status result{status_code::initialization_failed};
        std::lock_guard lock{mutex};
        fail_locked(result);
        return result;
    }
}

status source_frontend_generation::finish_discovery(
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    std::lock_guard lock{mutex};

    if (!failure.ok()) {
        return failure;
    }

    if (!discovery_queue.empty() ||
        active_discoveries != 0) {
        return {
            status_code::invalid_state
        };
    }

    const auto result =
        transaction->sources().validate_source_graph(
            operation,
            diagnostics);

    if (!result.ok()) {
        fail_locked(result);
        return result;
    }

    return {};
}

source_rebuild_result source_frontend_generation::rebuild(
    operation_id operation,
    diagnostic_buffer& diagnostics,
    source_acquisition_telemetry& telemetry,
    const project_builder& builder,
    const std::filesystem::path& checkpoint,
    metrics_store* checkpoint_metrics,
    const std::filesystem::path& compiled_checkpoint) noexcept {

    try {
        current_summary = {};

        for (const auto root :
             transaction->sources().roots()) {
            if (project_role_routing) {
                if (root.role == project_item_role::project) {
                    return status{status_code::configuration_failed};
                }

                std::lock_guard lock{mutex};
                auto& state = ensure(root.source);
                const auto implementation_only =
                    root.role == project_item_role::source;

                if (state.root_role_assigned &&
                    state.implementation_only != implementation_only) {
                    const status invalid{
                        status_code::configuration_failed
                    };
                    fail_locked(invalid);
                    return invalid;
                }

                state.root_role_assigned = true;
                state.implementation_only = implementation_only;
            }

            const auto result =
                enqueue(root.source);

            if (!result.ok()) {
                return result;
            }
        }

        const auto hardware_threads =
            std::thread::hardware_concurrency();

        // Source Manager is single-owner. Discovery proceeds in deterministic
        // waves: the coordinator prepares Source-local jobs, workers perform
        // only filesystem/hash work, then the coordinator applies results and
        // resolves include/DAG mutations before constructing the next wave.
        const auto available_discovery_workers =
            (std::min)(
                std::size_t{32},
                static_cast<std::size_t>(
                    hardware_threads == 0
                        ? 1
                        : hardware_threads));

        for (;;) {
            std::vector<source_id> wave_sources;

            {
                std::lock_guard lock{mutex};

                if (!failure.ok()) {
                    break;
                }

                while (!discovery_queue.empty()) {
                    wave_sources.push_back(discovery_queue.front());
                    discovery_queue.pop_front();
                }
            }

            if (wave_sources.empty()) {
                break;
            }

            current_summary.checked +=
                static_cast<std::uint32_t>(
                    wave_sources.size());

            std::sort(
                wave_sources.begin(),
                wave_sources.end(),
                [](source_id left, source_id right) noexcept {
                    return left.value() < right.value();
                });

            struct acquisition_slot {
                source_acquire_job job;
                source_acquire_result acquired;
                status result{};
            };

            const auto prepare_begin =
                std::chrono::steady_clock::now();

            std::vector<acquisition_slot> acquisitions(
                wave_sources.size());

            for (std::size_t index = 0;
                 index < wave_sources.size();
                 ++index) {
                auto& slot = acquisitions[index];
                slot.result = transaction->sources().prepare_acquire(
                    wave_sources[index],
                    slot.job);

                if (!slot.result.ok()) {
                    std::lock_guard lock{mutex};
                    fail_locked(slot.result);
                    break;
                }
            }

            current_summary.g0_acquire_prepare_ns +=
                g0_elapsed_ns(
                    prepare_begin,
                    std::chrono::steady_clock::now());

            {
                std::lock_guard lock{mutex};
                if (!failure.ok()) {
                    break;
                }
            }

            const auto acquire_execute_begin =
                std::chrono::steady_clock::now();

            const auto worker_count =
                (std::min)(
                    available_discovery_workers,
                    wave_sources.size());

            std::vector<source_acquisition_telemetry> local_telemetry;
            local_telemetry.reserve(worker_count);

            for (std::size_t index = 0;
                 index < worker_count;
                 ++index) {
                local_telemetry.emplace_back(telemetry.mode());
            }

            std::atomic_size_t next_index{0};
            std::vector<std::jthread> workers;
            workers.reserve(worker_count);

            for (std::size_t worker_index = 0;
                 worker_index < worker_count;
                 ++worker_index) {
                workers.emplace_back([&, worker_index]() {
                    for (;;) {
                        const auto index =
                            next_index.fetch_add(
                                1,
                                std::memory_order_relaxed);

                        if (index >=
                            acquisitions.size()) {
                            break;
                        }

                        auto& slot =
                            acquisitions[index];

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

            for (const auto& local : local_telemetry) {
                telemetry.merge_from(local);
            }

            current_summary.g0_acquire_execute_ns +=
                g0_elapsed_ns(
                    acquire_execute_begin,
                    std::chrono::steady_clock::now());

            bool wave_failed = false;

            // Validate worker results first in deterministic source_id order.
            for (std::size_t index = 0;
                 index < acquisitions.size();
                 ++index) {
                auto& slot = acquisitions[index];
                const auto source = wave_sources[index];

                if (slot.result.ok()) {
                    continue;
                }

                try {
                    diagnostics.emit({
                        diagnostics::source_acquisition_failed.id,
                        diagnostics::source_acquisition_failed.default_severity,
                        operation,
                        {source, 0, 0},
                        {}});
                }
                catch (...) {
                    slot.result = {
                        status_code::initialization_failed};
                }

                std::lock_guard lock{mutex};
                fail_locked(slot.result);
                wave_failed = true;
                break;
            }

            if (wave_failed) {
                break;
            }

            const auto apply_begin =
                std::chrono::steady_clock::now();

            // Coordinator applies all immutable worker results before parsing.
            // Candidate state is still detached, so a later discovery failure
            // cannot publish a partial generation.
            for (std::size_t index = 0;
                 index < acquisitions.size();
                 ++index) {
                auto& slot = acquisitions[index];
                const auto source = wave_sources[index];

                auto result = transaction->sources().apply_acquire(
                    slot.job,
                    std::move(slot.acquired),
                    telemetry);

                if (!result.ok()) {
                    try {
                        diagnostics.emit({
                            diagnostics::source_acquisition_failed.id,
                            diagnostics::source_acquisition_failed.default_severity,
                            operation,
                            {source, 0, 0},
                            {}});
                    }
                    catch (...) {
                        result = {status_code::initialization_failed};
                    }

                    std::lock_guard lock{mutex};
                    fail_locked(result);
                    wave_failed = true;
                    break;
                }
            }

            current_summary.g0_acquire_apply_ns +=
                g0_elapsed_ns(
                    apply_begin,
                    std::chrono::steady_clock::now());

            if (wave_failed) {
                break;
            }

            const auto discovery_begin =
                std::chrono::steady_clock::now();

            for (const auto source : wave_sources) {
                {
                    std::lock_guard lock{mutex};
                    ++active_discoveries;
                }

                const auto result = discover(
                    source,
                    operation,
                    diagnostics);

                if (!result.ok()) {
                    wave_failed = true;
                    break;
                }
            }

            current_summary.g0_discovery_ns +=
                g0_elapsed_ns(
                    discovery_begin,
                    std::chrono::steady_clock::now());

            if (wave_failed) {
                break;
            }
        }

        status result;

        {
            std::lock_guard lock{mutex};
            result = failure;

            if (result.ok() &&
                (!discovery_queue.empty() ||
                 active_discoveries != 0)) {
                result = {
                    status_code::invalid_state
                };
                fail_locked(result);
            }
        }

        if (!result.ok()) {
            return result;
        }

        const auto validation_begin =
            std::chrono::steady_clock::now();

        result =
            finish_discovery(
                operation,
                diagnostics);

        current_summary.g0_validation_ns +=
            g0_elapsed_ns(
                validation_begin,
                std::chrono::steady_clock::now());

        if (!result.ok()) {
            return result;
        }

        const auto semantic_begin =
            std::chrono::steady_clock::now();

        std::size_t semantic_count = 0;
        bool independent_semantic = true;
        std::vector<source_id> independent_sources;

        {
            std::lock_guard lock{mutex};

            if (semantic_scheduler_active) {
                return status{status_code::invalid_state};
            }

            independent_sources.reserve(states.size());

            for (std::size_t index = 0;
                 index < states.size();
                 ++index) {
                auto& state = states[index];

                if (!state.discovery_done ||
                    state.parsed) {
                    continue;
                }

                ++semantic_count;

                if (state.remaining != 0 ||
                    !state.dependencies.empty()) {
                    independent_semantic = false;
                    continue;
                }

                independent_sources.push_back(
                    source_id{
                        static_cast<std::uint32_t>(
                            index + 1)
                    });
            }

            current_summary.affected =
                static_cast<std::uint32_t>(
                    semantic_count);

            semantic_scheduler_active = true;
            semantic_remaining =
                static_cast<std::uint32_t>(
                    semantic_count);

            if (independent_semantic) {
                semantic_queue.clear();

                for (const auto source :
                     independent_sources) {
                    auto& state =
                        states[source.value() - 1];

                    state.semantic_queued = false;
                    state.parse_claimed = true;
                }
            }
        }



        const auto worker_count =
            semantic_count == 0
                ? std::size_t{0}
                : (std::min)(
                      semantic_count,
                      static_cast<std::size_t>(
                          hardware_threads == 0
                              ? 1
                              : hardware_threads));

        std::vector<diagnostic_buffer> worker_diagnostics(
            worker_count);
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);

        const auto worker =
            [&](std::size_t worker_index) {
                source_context context;

                const auto process =
                    [&](source_id source) -> bool {
                        const auto parse_result =
                            independent_semantic
                                ? parse_and_capture_independent_g0(
                                      source,
                                      operation,
                                      context)
                                : parse_and_capture(
                                      source,
                                      operation,
                                      context);

                        try {
                            auto& output =
                                worker_diagnostics[
                                    worker_index];

                            output.reserve(
                                output.records().size() +
                                context.diagnostics
                                    .records().size());

                            for (const auto& record :
                                 context.diagnostics
                                     .records()) {
                                output.emit(record);
                            }
                        }
                        catch (...) {
                            std::lock_guard lock{mutex};

                            fail_locked({
                                status_code::
                                    initialization_failed
                            });

                            return false;
                        }

                        context.reset();

                        if (!parse_result.ok()) {
                            std::lock_guard lock{mutex};

                            if (failure.ok()) {
                                fail_locked(
                                    parse_result);
                            }

                            return false;
                        }

                        return true;
                    };

                if (independent_semantic) {
                    const auto begin =
                        independent_sources.size() *
                        worker_index /
                        worker_count;

                    const auto end =
                        independent_sources.size() *
                        (worker_index + 1) /
                        worker_count;

                    for (auto index = begin;
                         index < end;
                         ++index) {
                        if (!process(
                                independent_sources[
                                    index])) {
                            break;
                        }
                    }

                    return;
                }

                for (;;) {
                    source_id source;

                    {
                        std::unique_lock lock{mutex};

                        semantic_condition.wait(
                            lock,
                            [&]() noexcept {
                                return
                                    !failure.ok() ||
                                    semantic_remaining == 0 ||
                                    !semantic_queue.empty();
                            });

                        if (!failure.ok() ||
                            semantic_remaining == 0) {
                            break;
                        }

                        source =
                            semantic_queue.front();

                        semantic_queue.pop_front();

                        auto& state =
                            states[
                                source.value() - 1];

                        state.semantic_queued = false;

                        if (state.parse_claimed ||
                            state.parsed) {
                            continue;
                        }

                        state.parse_claimed = true;
                    }

                    if (!process(source)) {
                        break;
                    }
                }
            };

        for (std::size_t index = 0;
             index < worker_count;
             ++index) {
            workers.emplace_back(worker, index);
        }

        workers.clear();

        {
            std::lock_guard lock{mutex};

            if (independent_semantic &&
                failure.ok()) {
                semantic_remaining = 0;
            }

            semantic_scheduler_active = false;

            if (failure.ok() && semantic_remaining != 0) {
                fail_locked({status_code::invalid_state});
            }

            result = failure;
            semantic_remaining = 0;
        }

        current_summary.g0_semantic_ns +=
            g0_elapsed_ns(
                semantic_begin,
                std::chrono::steady_clock::now());

        if (result.ok()) {
            const auto publish_begin =
                std::chrono::steady_clock::now();

            const auto publish_reserve_scan_begin =
                std::chrono::steady_clock::now();

            std::size_t string_reserve_hint = 0;
            std::size_t string_byte_reserve_hint = 0;
            std::size_t entity_reserve_hint = 0;
            std::size_t type_reserve_hint = 0;

            const auto maximum =
                (std::numeric_limits<std::size_t>::max)();

            for (const auto& state : states) {
                if (!state.parsed ||
                    !state.build_entry ||
                    state.published) {
                    continue;
                }

                const auto& entry =
                    *state.build_entry;

                if (entry.name_count() >
                        maximum -
                            string_reserve_hint ||
                    entry.name_bytes_size() >
                        maximum -
                            string_byte_reserve_hint) {
                    result = {
                        status_code::
                            initialization_failed
                    };
                    break;
                }

                string_reserve_hint +=
                    entry.name_count();

                string_byte_reserve_hint +=
                    entry.name_bytes_size();

                if (entry.enums.size() >
                        maximum - type_reserve_hint ||
                    entry.aggregates.size() >
                        maximum -
                            type_reserve_hint -
                            entry.enums.size()) {
                    result = {
                        status_code::
                            initialization_failed
                    };
                    break;
                }

                type_reserve_hint +=
                    entry.enums.size() +
                    entry.aggregates.size();

                if (entry.aggregates.size() >
                    maximum - entity_reserve_hint) {
                    result = {
                        status_code::
                            initialization_failed
                    };
                    break;
                }

                entity_reserve_hint +=
                    entry.aggregates.size();

                for (const auto& fact : entry.enums) {
                    if (fact.anonymous) {
                        continue;
                    }

                    if (entity_reserve_hint ==
                        maximum) {
                        result = {
                            status_code::
                                initialization_failed
                        };
                        break;
                    }

                    ++entity_reserve_hint;
                }

                if (!result.ok()) {
                    break;
                }
            }

            current_summary.g0_publish_reserve_scan_ns +=
                g0_elapsed_ns(
                    publish_reserve_scan_begin,
                    std::chrono::steady_clock::now());

            if (result.ok()) {
                const auto string_reserve_begin =
                    std::chrono::steady_clock::now();

                result =
                    transaction->strings().
                        reserve_bindings(
                            string_reserve_hint,
                            string_byte_reserve_hint);

                current_summary.g0_publish_string_reserve_ns +=
                    g0_elapsed_ns(
                        string_reserve_begin,
                        std::chrono::steady_clock::now());
            }

            if (result.ok()) {
                result =
                    transaction->graph_state().
                        reserve_rebuild(
                            states.size(),
                            string_reserve_hint,
                            entity_reserve_hint,
                            type_reserve_hint);
            }
            source_publish_scratch publish_scratch;
            publish_scratch.telemetry.enabled =
                telemetry.mode() == metrics_mode::detailed;

            // Canonical mutation is single-owner and deterministic. source_id is
            // the build-side ownership coordinate; worker completion order is
            // deliberately irrelevant to String/Entity/TypeRef allocation.
            for (std::size_t index = 0;
                 result.ok() && index < states.size();
                 ++index) {
                auto& state = states[index];

                if (!state.parsed || !state.build_entry || state.published) {
                    continue;
                }

                const auto publish_result = publish_source_entry(
                    *transaction,
                    *state.build_entry,
                    builder,
                    operation,
                    diagnostics,
                    publish_scratch);

                if (!publish_result.ok()) {
                    result = publish_result;
                    break;
                }

                state.published = true;
                ++state.counts.publish;
            }

            const auto publish_total_ns =
                g0_elapsed_ns(
                    publish_begin,
                    std::chrono::steady_clock::now());

            current_summary.g0_publish_ns +=
                publish_total_ns;

            if (publish_scratch.telemetry.enabled) {
                current_summary.g0_publish_source_replace_ns +=
                    publish_scratch.telemetry.source_replace_ns;
                current_summary.g0_publish_enum_name_ns +=
                    publish_scratch.telemetry.enum_name_ns;
                current_summary.g0_publish_enum_values_ns +=
                    publish_scratch.telemetry.enum_values_ns;
                current_summary.g0_publish_enum_sample_name_resolve_ns +=
                    publish_scratch.telemetry.enum_sample_name_resolve_ns;
                current_summary.g0_publish_enum_sample_name_intern_ns +=
                    publish_scratch.telemetry.enum_sample_name_intern_ns;
                current_summary.g0_publish_enum_sample_values_resolve_ns +=
                    publish_scratch.telemetry.enum_sample_values_resolve_ns;
                current_summary.g0_publish_enum_sample_values_intern_ns +=
                    publish_scratch.telemetry.enum_sample_values_intern_ns;
                current_summary.g0_publish_enum_builder_ns +=
                    publish_scratch.telemetry.enum_builder_ns;
                current_summary.g0_publish_enum_builder_value_copy_ns +=
                    publish_scratch.builder.enum_value_copy_ns;
                current_summary.g0_publish_enum_builder_graph_mutation_ns +=
                    publish_scratch.builder.enum_graph_mutation_ns;

                const auto& graph_sample =
                    publish_scratch.builder.named_enum;

                current_summary.g0_publish_enum_builder_graph_sample_total_ns +=
                    graph_sample.total_ns;

                current_summary.g0_publish_enum_builder_graph_sample_identity_ns +=
                    graph_sample.identity_ns;
                current_summary.g0_publish_enum_builder_graph_sample_contribution_build_ns +=
                    graph_sample.contribution_build_ns;
                current_summary.g0_publish_enum_builder_graph_sample_reconcile_ns +=
                    graph_sample.reconcile_ns;
                current_summary.g0_publish_enum_builder_graph_sample_delta_ns +=
                    graph_sample.delta_ns;
                current_summary.g0_publish_enum_builder_graph_sample_contribution_append_ns +=
                    graph_sample.contribution_append_ns;
                current_summary.g0_publish_enum_builder_graph_sample_materialize_ns +=
                    graph_sample.materialize_ns;
                current_summary.g0_publish_enum_builder_graph_sample_materialize_state_touch_ns +=
                    graph_sample.materialize_state_touch_ns;
                current_summary.g0_publish_enum_builder_graph_sample_materialize_type_storage_ns +=
                    graph_sample.materialize_type_storage_ns;
                current_summary.g0_publish_enum_builder_graph_sample_materialize_build_state_ns +=
                    graph_sample.materialize_build_state_ns;
                current_summary.g0_publish_enum_builder_graph_sample_materialize_assign_type_ns +=
                    graph_sample.materialize_assign_type_ns;

                current_summary.g0_publish_enum_builder_graph_sample_assign_type_handle_ns +=
                    graph_sample.assign_type_handle_ns;
                current_summary.g0_publish_enum_builder_graph_sample_assign_type_touch_type_ns +=
                    graph_sample.assign_type_touch_type_ns;
                current_summary.g0_publish_enum_builder_graph_sample_assign_type_candidate_store_ns +=
                    graph_sample.assign_type_candidate_store_ns;
                current_summary.g0_publish_enum_builder_graph_sample_assign_type_named_type_ref_ns +=
                    graph_sample.assign_type_named_type_ref_ns;

                current_summary.g0_publish_enum_builder_graph_sample_named_type_ref_existing_lookup_ns +=
                    graph_sample.named_type_ref_existing_lookup_ns;
                current_summary.g0_publish_enum_builder_graph_sample_named_type_ref_canonical_append_ns +=
                    graph_sample.named_type_ref_canonical_append_ns;
                current_summary.g0_publish_enum_builder_graph_sample_named_type_ref_mapping_append_ns +=
                    graph_sample.named_type_ref_mapping_append_ns;
                current_summary.g0_publish_enum_builder_graph_sample_named_type_ref_index_emplace_ns +=
                    graph_sample.named_type_ref_index_emplace_ns;

                current_summary.g0_publish_enum_builder_graph_sample_materialize_attach_ns +=
                    graph_sample.materialize_attach_ns;

                current_summary.g0_publish_enum_builder_graph_call_count +=
                    graph_sample.calls;
                current_summary.g0_publish_enum_builder_graph_sample_count +=
                    graph_sample.samples;

                const auto enum_builder_attributed_ns =
                    publish_scratch.builder.enum_value_copy_ns +
                    publish_scratch.builder.enum_graph_mutation_ns;

                if (publish_scratch.telemetry.enum_builder_ns >=
                    enum_builder_attributed_ns) {
                    current_summary.g0_publish_enum_builder_residual_ns +=
                        publish_scratch.telemetry.enum_builder_ns -
                        enum_builder_attributed_ns;
                }

                current_summary.g0_publish_aggregate_builder_ns +=
                    publish_scratch.telemetry.aggregate_builder_ns;

                current_summary.g0_publish_source_count +=
                    publish_scratch.telemetry.source_count;
                current_summary.g0_publish_enum_builder_count +=
                    publish_scratch.telemetry.enum_builder_count;
                current_summary.g0_publish_aggregate_builder_count +=
                    publish_scratch.telemetry.aggregate_builder_count;

                const auto attributed_ns =
                    current_summary.g0_publish_reserve_scan_ns +
                    current_summary.g0_publish_string_reserve_ns +
                    publish_scratch.telemetry.source_replace_ns +
                    // RC18 sampled enum preparation is not subtracted from wall residual.
                    publish_scratch.telemetry.enum_builder_ns +
                    publish_scratch.telemetry.aggregate_builder_ns;

                if (publish_total_ns >= attributed_ns) {
                    current_summary.g0_publish_residual_ns +=
                        publish_total_ns - attributed_ns;
                }
            }
        }

        try {
            std::size_t additional = 0;

            for (const auto& buffer : worker_diagnostics) {
                additional += buffer.records().size();
            }

            diagnostics.reserve(
                diagnostics.records().size() + additional);

            for (const auto& buffer : worker_diagnostics) {
                for (const auto& record : buffer.records()) {
                    diagnostics.emit(record);
                }
            }

            diagnostics.sort_deterministic();
        }
        catch (...) {
            const status diagnostic_failure{
                status_code::initialization_failed
            };

            std::lock_guard lock{mutex};
            fail_locked(diagnostic_failure);
            return diagnostic_failure;
        }

        if (!result.ok()) {
            return result;
        }

        const auto commit_begin =
            std::chrono::steady_clock::now();

        result =
            transaction->commit();

        current_summary.g0_commit_ns +=
            g0_elapsed_ns(
                commit_begin,
                std::chrono::steady_clock::now());

        const auto& transaction_timing =
            transaction->timing();

        current_summary.g0_tx_source_prepare_ns =
            transaction_timing.source_prepare_ns;
        current_summary.g0_tx_string_prepare_ns =
            transaction_timing.string_prepare_ns;
        current_summary.g0_tx_graph_prepare_ns =
            transaction_timing.graph_prepare_ns;

        current_summary.g0_graph_pending_member_resolution_ns =
            transaction_timing.graph_pending_member_resolution_ns;
        current_summary.g0_graph_live_typeref_validation_ns =
            transaction_timing.graph_live_typeref_validation_ns;
        current_summary.g0_graph_canonical_typeref_rebuild_ns =
            transaction_timing.graph_canonical_typeref_rebuild_ns;
        current_summary.g0_graph_string_validation_ns =
            transaction_timing.graph_string_validation_ns;
        current_summary.g0_graph_definition_scan_ns =
            transaction_timing.graph_definition_scan_ns;
        current_summary.g0_graph_definition_materialization_ns =
            transaction_timing.graph_definition_materialization_ns;
        current_summary.g0_graph_rebuild_storage_ns =
            transaction_timing.graph_rebuild_storage_ns;
        current_summary.g0_graph_dependency_index_ns =
            transaction_timing.graph_dependency_index_ns;
        current_summary.g0_graph_final_prepare_ns =
            transaction_timing.graph_final_prepare_ns;

        current_summary.g0_tx_string_retention_ns =
            transaction_timing.string_retention_ns;
        current_summary.g0_tx_string_compaction_ns =
            transaction_timing.string_compaction_ns;
        current_summary.g0_tx_contribution_prepare_ns =
            transaction_timing.contribution_prepare_ns;

        current_summary.g0_tx_source_publish_ns =
            transaction_timing.source_publish_ns;
        current_summary.g0_tx_string_publish_ns =
            transaction_timing.string_publish_ns;
        current_summary.g0_tx_contribution_publish_ns =
            transaction_timing.contribution_publish_ns;
        current_summary.g0_tx_graph_publish_ns =
            transaction_timing.graph_publish_ns;

        source_rebuild_result completed;
        completed.semantic = result;
        completed.frontend = summary();

        if (!result.ok() ||
            (checkpoint.empty() &&
             compiled_checkpoint.empty())) {
            return completed;
        }

        if (!checkpoint.empty()) {
            completed.checkpoint =
                transaction->checkpoint_sources(
                    checkpoint,
                    checkpoint_metrics);
        }

        if (completed.checkpoint &&
            !completed.checkpoint->ok()) {
            try {
                diagnostics.emit({
                    diagnostics::source_checkpoint_save_failed.id,
                    diagnostics::source_checkpoint_save_failed.default_severity,
                    operation,
                    {},
                    {}
                });
            }
            catch (...) {
                // Checkpoint diagnostics are best-effort after semantic commit.
            }
        }

        if (!compiled_checkpoint.empty()) {
            completed.compiled_checkpoint =
                transaction->checkpoint_compiled(
                    compiled_checkpoint,
                    checkpoint_metrics);
        }

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

source_frontend_summary source_frontend_generation::summary() const noexcept {
    std::lock_guard lock{mutex};

    auto result = current_summary;

    result.discovery = 0;
    result.lex = 0;
    result.parse = 0;
    result.publish = 0;
    result.working_states =
        static_cast<std::uint32_t>(
            cache != nullptr
                ? sparse_states.size()
                : states.size());

    const auto accumulate =
        [&](const source_state& state) noexcept {
            result.discovery += state.counts.discovery;
            result.lex += state.counts.lex;
            result.parse += state.counts.parse;
            result.publish += state.counts.publish;
        };

    if (cache != nullptr) {
        for (const auto& [source, state] :
             sparse_states) {
            (void)source;
            accumulate(state);
        }
    }
    else {
        for (const auto& state : states) {
            accumulate(state);
        }
    }

    return result;
}

source_frontend_counts source_frontend_generation::counts(
    source_id source) const noexcept {

    std::lock_guard lock{mutex};

    const auto* state =
        find_state(source);

    return state != nullptr
        ? state->counts
        : source_frontend_counts{};
}

std::uint32_t source_frontend_generation::remaining_dependencies(
    source_id source) const noexcept {

    std::lock_guard lock{mutex};

    const auto* state =
        find_state(source);

    return state != nullptr
        ? state->remaining
        : 0;
}

bool source_frontend_generation::published(
    source_id source) const noexcept {

    std::lock_guard lock{mutex};

    const auto* state =
        find_state(source);

    return state != nullptr &&
        state->published;
}

const source_environment_storage*
source_frontend_generation::interface(
    source_id source) const noexcept {

    std::lock_guard lock{mutex};

    const auto* state =
        find_state(source);

    if (state == nullptr) {
        return nullptr;
    }

    return state->interface
        ? state->interface.get()
        : state->cached_interface;
}

bool source_frontend_generation::failed() const noexcept {
    std::lock_guard lock{mutex};
    return !failure.ok();
}

} // namespace cw::server
