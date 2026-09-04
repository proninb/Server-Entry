#include "source_frontend_generation.hpp"

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
#include <iterator>
#include <thread>

namespace cw::server {
namespace {

// Builds the immutable Parser-visible interface exported by one parsed Source.
// All string_views supplied to initialize() refer to source_context storage only
// for the duration of this function; source_environment_storage copies them.
status build_interface(
    const source_context& context,
    source_environment_storage& output,
    std::span<const source_environment_storage* const> imports) noexcept {

    try {
        std::vector<source_constant_binding> constants;
        std::vector<source_type_binding> types;

        for (const auto& fact : context.enums) {
            std::string_view scope;

            if (fact.scope_name &&
                !context.resolve_name(
                    fact.scope_name,
                    scope).ok()) {
                return {
                    status_code::configuration_failed
                };
            }

            std::string_view canonical;

            if (!fact.anonymous) {
                if (!context.resolve_name(
                        fact.canonical_name,
                        canonical).ok()) {
                    return {
                        status_code::configuration_failed
                    };
                }

                const auto separator =
                    canonical.rfind("::");

                const auto local =
                    separator == std::string_view::npos
                        ? canonical
                        : canonical.substr(separator + 2);

                types.push_back({
                    scope,
                    local,
                    canonical
                });
            }

            const auto value_scope =
                fact.scoped
                    ? canonical
                    : scope;

            for (const auto& value :
                 context.enumerators(fact)) {
                std::string_view name;

                if (!context.resolve_name(
                        value.name,
                        name).ok()) {
                    return {
                        status_code::configuration_failed
                    };
                }

                constants.push_back({
                    value_scope,
                    name,
                    value.value
                });
            }
        }

        for (const auto& fact : context.aggregates) {
            std::string_view scope;
            std::string_view canonical;

            if (fact.scope_name &&
                !context.resolve_name(
                    fact.scope_name,
                    scope).ok()) {
                return {
                    status_code::configuration_failed
                };
            }

            if (!context.resolve_name(
                    fact.canonical_name,
                    canonical).ok()) {
                return {
                    status_code::configuration_failed
                };
            }

            const auto separator =
                canonical.rfind("::");

            const auto local =
                separator == std::string_view::npos
                    ? canonical
                    : canonical.substr(separator + 2);

            types.push_back({
                scope,
                local,
                canonical
            });
        }

        return output.initialize(
            constants,
            types,
            imports);
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
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
    if (states.size() < source.value()) {
        states.resize(source.value());
    }

    return states[source.value() - 1];
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

        if (source.value() > states.size() ||
            !states[source.value() - 1]
                 .discovery_claimed) {
            if (active_discoveries != 0) {
                --active_discoveries;
            }

            discovery_condition.notify_all();
            return {
                status_code::invalid_state
            };
        }

        if (states[source.value() - 1]
                .discovery_done) {
            if (active_discoveries != 0) {
                --active_discoveries;
            }

            discovery_condition.notify_all();
            return {};
        }

        for (const auto dependency : dependencies) {
            ensure(dependency);

            auto& dependency_state =
                states[dependency.value() - 1];

            if (!dependency_state.published) {
                ++states[source.value() - 1]
                      .remaining;
            }

            dependency_state.dependents.push_back(
                source);

            if (!dependency_state.discovery_claimed) {
                dependency_state.discovery_claimed = true;
                discovery_queue.push_back(dependency);
                discovery_condition.notify_one();
            }
        }

        auto& state =
            states[source.value() - 1];

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

    auto& state =
        states[source.value() - 1];

    state.semantic_queued = false;

    if (state.parse_claimed ||
        state.parsed) {
        source = {};
        return false;
    }

    state.parse_claimed = true;
    return true;
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

        {
            std::lock_guard lock{mutex};

            if (!failure.ok()) {
                return failure;
            }

            if (!source || source.value() > states.size()) {
                return {status_code::invalid_state};
            }

            const auto& state = states[source.value() - 1];

            if (!state.parse_claimed ||
                state.parsed ||
                !state.discovery_done ||
                state.remaining != 0) {
                return {status_code::invalid_state};
            }

            tokens = state.tokens;
            dependency_interfaces.reserve(state.dependencies.size());

            for (const auto dependency : state.dependencies) {
                const auto* dependency_interface =
                    states[dependency.value() - 1].interface.get();

                if (!dependency_interface) {
                    return {status_code::invalid_state};
                }

                dependency_interfaces.push_back(dependency_interface);
            }

            visible_imports.reserve(state.visibility.size());

            for (const auto& item : state.visibility) {
                const auto* dependency_interface =
                    states[item.dependency.value() - 1].interface.get();

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

        auto published_interface =
            std::make_unique<source_environment_storage>();

        result = build_interface(
            context,
            *published_interface,
            dependency_interfaces);

        if (!result.ok()) {
            std::lock_guard lock{mutex};
            fail_locked(result);
            return result;
        }

        auto build_entry = std::make_unique<source_build_entry>();

        const parser_source_fact_batch batch{
            source,
            &context,
            context.enums,
            context.aggregates
        };

        result = capture_source_facts(batch, *build_entry);

        if (!result.ok()) {
            std::lock_guard lock{mutex};
            fail_locked(result);
            return result;
        }

        // From this point the Parser context is no longer referenced by build
        // state. Only source_id-owned interface/contribution data is published.
        std::lock_guard lock{mutex};
        auto& state = states[source.value() - 1];

        state.interface = std::move(published_interface);
        state.build_entry = std::move(build_entry);
        state.parsed = true;
        ++state.counts.parse;

        if (semantic_scheduler_active && semantic_remaining != 0) {
            --semantic_remaining;
        }

        for (const auto dependent_id : state.dependents) {
            auto& dependent = states[dependent_id.value() - 1];

            if (dependent.remaining != 0) {
                --dependent.remaining;
            }

            enqueue_ready_locked(dependent_id, dependent);
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
        for (const auto root :
             transaction->sources().roots()) {
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

            {
                std::lock_guard lock{mutex};
                if (!failure.ok()) {
                    break;
                }
            }

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
                        const auto index = next_index.fetch_add(
                            1,
                            std::memory_order_relaxed);

                        if (index >= acquisitions.size()) {
                            break;
                        }

                        auto& slot = acquisitions[index];
                        slot.result = source_manager_update::execute_acquire(
                            slot.job,
                            local_telemetry[worker_index],
                            slot.acquired);
                    }
                });
            }

            workers.clear();

            for (const auto& local : local_telemetry) {
                telemetry.merge_from(local);
            }

            bool wave_failed = false;

            for (std::size_t index = 0;
                 index < acquisitions.size();
                 ++index) {
                auto& slot = acquisitions[index];
                const auto source = wave_sources[index];

                if (!slot.result.ok()) {
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

                {
                    std::lock_guard lock{mutex};
                    ++active_discoveries;
                }

                result = discover(
                    source,
                    operation,
                    diagnostics);

                if (!result.ok()) {
                    wave_failed = true;
                    break;
                }
            }

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

        result =
            finish_discovery(
                operation,
                diagnostics);

        if (!result.ok()) {
            return result;
        }

        std::size_t semantic_count = 0;

        {
            std::lock_guard lock{mutex};

            if (semantic_scheduler_active) {
                return status{status_code::invalid_state};
            }

            for (const auto& state : states) {
                if (state.discovery_done && !state.parsed) {
                    ++semantic_count;
                }
            }

            semantic_scheduler_active = true;
            semantic_remaining =
                static_cast<std::uint32_t>(semantic_count);
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

                        source = semantic_queue.front();
                        semantic_queue.pop_front();

                        auto& state =
                            states[source.value() - 1];

                        state.semantic_queued = false;

                        if (state.parse_claimed ||
                            state.parsed) {
                            continue;
                        }

                        state.parse_claimed = true;
                    }

                    const auto parse_result =
                        parse_and_capture(
                            source,
                            operation,
                            context);

                    try {
                        auto& output =
                            worker_diagnostics[worker_index];

                        output.reserve(
                            output.records().size() +
                            context.diagnostics.records().size());

                        for (const auto& record :
                             context.diagnostics.records()) {
                            output.emit(record);
                        }
                    }
                    catch (...) {
                        std::lock_guard lock{mutex};
                        fail_locked({
                            status_code::initialization_failed
                        });
                        break;
                    }

                    context.reset();

                    if (!parse_result.ok()) {
                        std::lock_guard lock{mutex};

                        if (failure.ok()) {
                            fail_locked(parse_result);
                        }

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
            semantic_scheduler_active = false;

            if (failure.ok() && semantic_remaining != 0) {
                fail_locked({status_code::invalid_state});
            }

            result = failure;
            semantic_remaining = 0;
        }

        if (result.ok()) {
            // Canonical mutation is single-owner and deterministic. source_id is
            // the build-side ownership coordinate; worker completion order is
            // deliberately irrelevant to String/Entity/TypeRef allocation.
            for (std::size_t index = 0; index < states.size(); ++index) {
                auto& state = states[index];

                if (!state.parsed || !state.build_entry || state.published) {
                    continue;
                }

                const auto publish_result = publish_source_entry(
                    *transaction,
                    *state.build_entry,
                    builder,
                    operation,
                    diagnostics);

                if (!publish_result.ok()) {
                    result = publish_result;
                    break;
                }

                state.published = true;
                ++state.counts.publish;
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

        result =
            transaction->commit();

        if (!result.ok() ||
            (checkpoint.empty() &&
             compiled_checkpoint.empty())) {
            return result;
        }

        source_rebuild_result completed;
        completed.semantic = result;

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

source_frontend_counts source_frontend_generation::counts(
    source_id source) const noexcept {

    std::lock_guard lock{mutex};

    return
        source &&
        source.value() <= states.size()
            ? states[source.value() - 1].counts
            : source_frontend_counts{};
}

std::uint32_t source_frontend_generation::remaining_dependencies(
    source_id source) const noexcept {

    std::lock_guard lock{mutex};

    return
        source &&
        source.value() <= states.size()
            ? states[source.value() - 1].remaining
            : 0;
}

bool source_frontend_generation::published(
    source_id source) const noexcept {

    std::lock_guard lock{mutex};

    return
        source &&
        source.value() <= states.size() &&
        states[source.value() - 1].published;
}

const source_environment_storage*
source_frontend_generation::interface(
    source_id source) const noexcept {

    std::lock_guard lock{mutex};

    return
        source &&
        source.value() <= states.size()
            ? states[source.value() - 1]
                  .interface.get()
            : nullptr;
}

bool source_frontend_generation::failed() const noexcept {
    std::lock_guard lock{mutex};
    return !failure.ok();
}

} // namespace cw::server
