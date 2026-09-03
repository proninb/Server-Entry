#include "source_frontend_generation.hpp"

#include "source_publisher.hpp"
#include "../builder/project_builder.hpp"
#include "../graph/graph_build_transaction.hpp"
#include "../parser/parser.hpp"
#include "../parser/source_context.hpp"
#include "../parser/source_environment.hpp"
#include "../source/source_manager.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"
#include "../../metrics/source_acquisition_telemetry.hpp"

#include <algorithm>

namespace cw::server
{
namespace
{
status build_interface(const source_context& context,
                       source_environment_storage& output) noexcept
{
    try
    {
        std::vector<source_constant_binding> constants;
        std::vector<source_type_binding> types;
        for (const auto& fact : context.enums)
        {
            std::string_view scope;
            if (fact.scope_name && !context.resolve_name(fact.scope_name, scope).ok())
                return {status_code::configuration_failed};
            std::string_view canonical;
            if (!fact.anonymous)
            {
                if (!context.resolve_name(fact.canonical_name, canonical).ok())
                    return {status_code::configuration_failed};
                const auto separator = canonical.rfind("::");
                const auto local = separator == std::string_view::npos
                    ? canonical : canonical.substr(separator + 2);
                types.push_back({scope, local, canonical});
            }
            const auto value_scope = fact.scoped ? canonical : scope;
            for (const auto& value : context.enumerators(fact))
            {
                std::string_view name;
                if (!context.resolve_name(value.name, name).ok())
                    return {status_code::configuration_failed};
                constants.push_back({value_scope, name, value.value});
            }
        }
        for (const auto& fact : context.aggregates)
        {
            std::string_view scope, canonical;
            if (fact.scope_name && !context.resolve_name(fact.scope_name, scope).ok())
                return {status_code::configuration_failed};
            if (!context.resolve_name(fact.canonical_name, canonical).ok())
                return {status_code::configuration_failed};
            const auto separator = canonical.rfind("::");
            const auto local = separator == std::string_view::npos
                ? canonical : canonical.substr(separator + 2);
            types.push_back({scope, local, canonical});
        }
        return output.initialize(constants, types);
    }
    catch (...) { return {status_code::initialization_failed}; }
}
}

source_frontend_generation::source_frontend_generation(
    graph_build_transaction& transaction,
    language_configuration language) noexcept
    : transaction_(&transaction), backend_(&default_parser_backend()),
      language_(language) {}

source_frontend_generation::source_frontend_generation(
    graph_build_transaction& transaction, const parser_backend& backend,
    language_configuration language) noexcept
    : transaction_(&transaction), backend_(&backend), language_(language) {}

source_frontend_generation::source_state&
source_frontend_generation::ensure(source_id source)
{
    if (states_.size() < source.value()) states_.resize(source.value());
    return states_[source.value() - 1];
}

void source_frontend_generation::fail_locked(status result) noexcept
{
    if (failure_.ok()) failure_ = result;
    discovery_queue_.clear();
    semantic_queue_.clear();
    transaction_->fail(failure_);
}

void source_frontend_generation::enqueue_ready_locked(
    source_id source, source_state& state)
{
    if (failure_.ok() && state.discovery_done && state.remaining == 0 &&
        !state.parse_claimed && !state.semantic_queued && !state.published)
    {
        state.semantic_queued = true;
        semantic_queue_.push_back(source);
    }
}

status source_frontend_generation::enqueue(source_id source) noexcept
{
    if (!source || !transaction_) return {status_code::invalid_state};
    try
    {
        std::lock_guard lock{mutex_};
        if (!failure_.ok()) return failure_;
        auto& state = ensure(source);
        if (!state.discovery_claimed)
        {
            state.discovery_claimed = true;
            discovery_queue_.push_back(source);
        }
        return {};
    }
    catch (...) { return {status_code::initialization_failed}; }
}

bool source_frontend_generation::take_discovery(source_id& source) noexcept
{
    source = {};
    std::lock_guard lock{mutex_};
    if (!failure_.ok() || discovery_queue_.empty()) return false;
    source = discovery_queue_.front();
    discovery_queue_.pop_front();
    ++active_discoveries_;
    return true;
}

status source_frontend_generation::discover(
    source_id source, operation_id operation, diagnostic_buffer& diagnostics) noexcept
{
    const auto fail = [&](status result) noexcept
    {
        std::lock_guard lock{mutex_};
        if (active_discoveries_ != 0) --active_discoveries_;
        fail_locked(result);
        return result;
    };
    source_view view;
    status result;
    {
        std::lock_guard lock{mutex_};
        result = transaction_->sources().get_view(source, view);
    }
    if (!result.ok()) return fail(result);

    std::vector<parser_token> tokens;
    std::vector<directive_span> directives;
    result = lex_source(view, operation, diagnostics, tokens, &directives);
    if (!result.ok()) return fail(result);
    try
    {
        std::vector<parser_token> semantic_tokens;
        std::vector<std::string_view> include_names;
        std::vector<bool> namespace_braces;
        semantic_tokens.reserve(tokens.size());
        include_names.reserve(directives.size());
        std::size_t cursor = 0;
        std::uint32_t namespace_depth = 0;
        bool namespace_pending = false;
        const auto scan_scopes = [&](std::size_t begin, std::size_t end)
        {
            for (auto index = begin; index < end; ++index)
            {
                const auto& token = tokens[index];
                if (token.kind == parser_token_kind::keyword_namespace)
                    namespace_pending = true;
                else if (token.kind == parser_token_kind::punctuation &&
                         token.punctuation == parser_punctuation::left_brace)
                {
                    namespace_braces.push_back(namespace_pending);
                    if (namespace_pending) ++namespace_depth;
                    namespace_pending = false;
                }
                else if (token.kind == parser_token_kind::punctuation &&
                         token.punctuation == parser_punctuation::right_brace)
                {
                    if (!namespace_braces.empty())
                    {
                        if (namespace_braces.back()) --namespace_depth;
                        namespace_braces.pop_back();
                    }
                    namespace_pending = false;
                }
                else if (token.kind == parser_token_kind::punctuation &&
                         token.punctuation == parser_punctuation::semicolon)
                    namespace_pending = false;
            }
        };
        const auto emit_error = [&](const diagnostic_descriptor& descriptor,
                                    const parser_token& token) noexcept
        {
            try
            {
                diagnostics.emit({descriptor.id, descriptor.default_severity,
                    operation, {source, token.offset, token.length}, {}});
            }
            catch (...) { return fail({status_code::initialization_failed}); }
            return fail({status_code::configuration_failed});
        };
        for (const auto span : directives)
        {
            if (span.token_begin < cursor || span.token_end > tokens.size() ||
                span.token_end - span.token_begin < 2)
                return fail({status_code::configuration_failed});
            scan_scopes(cursor, span.token_begin);
            semantic_tokens.insert(semantic_tokens.end(), tokens.begin() + cursor,
                                   tokens.begin() + span.token_begin);
            const auto& marker = tokens[span.token_begin];
            const auto& directive = tokens[span.token_begin + 1];
            const auto directive_name =
                view.bytes.substr(directive.offset, directive.length);
            if (marker.punctuation != parser_punctuation::hash ||
                directive.kind != parser_token_kind::identifier ||
                directive_name != "include" || !language_.preprocessor.include)
                return emit_error(diagnostics::source_unsupported_directive,
                                  directive);
            if (span.token_end - span.token_begin != 3)
                return emit_error(diagnostics::parser_invalid_source, directive);
            const auto& argument = tokens[span.token_begin + 2];
            if (argument.kind != parser_token_kind::string_literal ||
                argument.length < 2)
                return emit_error(diagnostics::parser_invalid_source, argument);
            if (namespace_depth != 0)
                return emit_error(diagnostics::source_include_inside_namespace,
                                  marker);
            include_names.push_back(view.bytes.substr(argument.offset + 1,
                                                       argument.length - 2));
            cursor = span.token_end;
        }
        semantic_tokens.insert(semantic_tokens.end(), tokens.begin() + cursor,
                               tokens.end());

        std::lock_guard lock{mutex_};
        if (!failure_.ok()) return failure_;
        if (source.value() > states_.size() ||
            !states_[source.value() - 1].discovery_claimed)
            return {status_code::invalid_state};
        if (states_[source.value() - 1].discovery_done) return {};

        std::vector<source_id> dependencies;
        dependencies.reserve(include_names.size());
        for (const auto name : include_names)
        {
            source_id dependency;
            result = transaction_->sources().resolve_include(source, name, dependency);
            if (!result.ok())
            {
                try
                {
                    diagnostics.emit({diagnostics::source_include_not_found.id,
                        diagnostics::source_include_not_found.default_severity,
                        operation, {source, 0, 0}, {}});
                }
                catch (...)
                {
                    if (active_discoveries_ != 0) --active_discoveries_;
                    fail_locked({status_code::initialization_failed});
                    return {status_code::initialization_failed};
                }
                if (active_discoveries_ != 0) --active_discoveries_;
                fail_locked(result);
                return result;
            }
            if (std::find(dependencies.begin(), dependencies.end(), dependency) !=
                dependencies.end()) continue;
            dependencies.push_back(dependency);
            ensure(dependency);
            auto& dependency_state = states_[dependency.value() - 1];
            if (!dependency_state.published)
                ++states_[source.value() - 1].remaining;
            dependency_state.dependents.push_back(source);
            if (!dependency_state.discovery_claimed)
            {
                dependency_state.discovery_claimed = true;
                discovery_queue_.push_back(dependency);
            }
        }
        result = transaction_->sources().set_includes(source, dependencies);
        if (!result.ok())
        {
            if (active_discoveries_ != 0) --active_discoveries_;
            fail_locked(result);
            return result;
        }
        auto& state = states_[source.value() - 1];
        state.dependencies = std::move(dependencies);
        state.tokens = std::move(semantic_tokens);
        state.discovery_done = true;
        ++state.counts.discovery;
        ++state.counts.lex;
        if (active_discoveries_ != 0) --active_discoveries_;
        enqueue_ready_locked(source, state);
        return {};
    }
    catch (...) { return fail({status_code::initialization_failed}); }
}

bool source_frontend_generation::take_semantic_ready(source_id& source) noexcept
{
    source = {};
    std::lock_guard lock{mutex_};
    if (!failure_.ok() || semantic_queue_.empty()) return false;
    source = semantic_queue_.front();
    semantic_queue_.pop_front();
    auto& state = states_[source.value() - 1];
    state.semantic_queued = false;
    if (state.parse_claimed || state.published) { source = {}; return false; }
    state.parse_claimed = true;
    return true;
}

status source_frontend_generation::parse_and_publish(
    source_id source, operation_id operation, source_context& context,
    const project_builder& builder) noexcept
{
    try
    {
    source_view view;
    status result;
    {
        std::lock_guard lock{mutex_};
        result = transaction_->sources().get_view(source, view);
    }
    if (!result.ok())
    {
        std::lock_guard lock{mutex_};
        fail_locked(result);
        return result;
    }
    std::span<const parser_token> tokens;
    std::vector<const source_environment_storage*> dependency_interfaces;
    {
        std::lock_guard lock{mutex_};
        if (!failure_.ok()) return failure_;
        if (!source || source.value() > states_.size()) return {status_code::invalid_state};
        const auto& state = states_[source.value() - 1];
        if (!state.parse_claimed || !state.discovery_done || state.remaining != 0)
            return {status_code::invalid_state};
        tokens = state.tokens;
        dependency_interfaces.reserve(state.dependencies.size());
        for (const auto dependency : state.dependencies)
        {
            const auto* interface = states_[dependency.value() - 1].interface.get();
            if (!interface) return {status_code::invalid_state};
            dependency_interfaces.push_back(interface);
        }
    }
    const source_environment environment{dependency_interfaces};
    result = backend_->parse(view, tokens, environment, language_, operation,
                             context);
    if (!result.ok())
    {
        std::lock_guard lock{mutex_};
        fail_locked(result);
        return result;
    }
    {
        std::lock_guard lock{mutex_};
        ++states_[source.value() - 1].counts.parse;
    }
    auto published_interface = std::make_unique<source_environment_storage>();
    result = build_interface(context, *published_interface);
    if (!result.ok())
    {
        std::lock_guard lock{mutex_};
        fail_locked(result);
        return result;
    }
    const parser_source_fact_batch batch{
        source, &context, context.enums, context.aggregates};
    {
        std::lock_guard publication_lock{publication_mutex_};
        result = publish_source_facts(*transaction_, batch, builder, operation,
                                      context.diagnostics);
    }
    std::lock_guard lock{mutex_};
    if (!result.ok()) { fail_locked(result); return result; }
    auto& state = states_[source.value() - 1];
    state.interface = std::move(published_interface);
    state.published = true;
    ++state.counts.publish;
    for (const auto dependent_id : state.dependents)
    {
        auto& dependent = states_[dependent_id.value() - 1];
        if (dependent.remaining != 0) --dependent.remaining;
        enqueue_ready_locked(dependent_id, dependent);
    }
    return {};
    }
    catch (...)
    {
        const status result{status_code::initialization_failed};
        std::lock_guard lock{mutex_};
        fail_locked(result);
        return result;
    }
}

status source_frontend_generation::finish_discovery(
    operation_id operation, diagnostic_buffer& diagnostics) noexcept
{
    std::lock_guard lock{mutex_};
    if (!failure_.ok()) return failure_;
    if (!discovery_queue_.empty() || active_discoveries_ != 0)
        return {status_code::invalid_state};
    const auto result = transaction_->sources().validate_source_graph(operation,
                                                                       diagnostics);
    if (!result.ok()) { fail_locked(result); return result; }
    return {};
}

source_rebuild_result source_frontend_generation::rebuild(
    operation_id operation, diagnostic_buffer& diagnostics,
    source_acquisition_telemetry& telemetry,
    const project_builder& builder,
    const std::filesystem::path& checkpoint,
    metrics_store* checkpoint_metrics,
    const std::filesystem::path& compiled_checkpoint) noexcept
{
    try
    {
        for (const auto root : transaction_->sources().roots())
        {
            const auto result = enqueue(root.source);
            if (!result.ok()) return result;
        }

        source_id work;
        while (take_discovery(work))
        {
            auto result = transaction_->sources().acquire(work, telemetry);
            if (!result.ok())
            {
                try
                {
                    diagnostics.emit({diagnostics::source_acquisition_failed.id,
                        diagnostics::source_acquisition_failed.default_severity,
                        operation, {work, 0, 0}, {}});
                }
                catch (...) { result = {status_code::initialization_failed}; }
                std::lock_guard lock{mutex_};
                if (active_discoveries_ != 0) --active_discoveries_;
                fail_locked(result);
                return result;
            }
            result = discover(work, operation, diagnostics);
            if (!result.ok()) return result;
        }

        auto result = finish_discovery(operation, diagnostics);
        if (!result.ok()) return result;

        source_context context;
        while (take_semantic_ready(work))
        {
            result = parse_and_publish(work, operation, context, builder);
            if (!result.ok())
            {
                diagnostics.reserve(diagnostics.records().size() +
                                    context.diagnostics.records().size());
                for (const auto& record : context.diagnostics.records())
                    diagnostics.emit(record);
                return result;
            }
            context.reset();
        }
        result = transaction_->commit();
        if (!result.ok() || (checkpoint.empty() && compiled_checkpoint.empty())) return result;

        source_rebuild_result completed;
        completed.semantic = result;
        if (!checkpoint.empty()) completed.checkpoint = transaction_->checkpoint_sources(
            checkpoint, checkpoint_metrics);
        if (completed.checkpoint && !completed.checkpoint->ok())
        {
            try
            {
                diagnostics.emit({diagnostics::source_checkpoint_save_failed.id,
                    diagnostics::source_checkpoint_save_failed.default_severity,
                    operation, {}, {}});
            }
            catch (...) {}
        }
        if (!compiled_checkpoint.empty())
            completed.compiled_checkpoint = transaction_->checkpoint_compiled(
                compiled_checkpoint, checkpoint_metrics);
        return completed;
    }
    catch (...)
    {
        const status result{status_code::initialization_failed};
        std::lock_guard lock{mutex_};
        fail_locked(result);
        return result;
    }
}

source_frontend_counts source_frontend_generation::counts(source_id source) const noexcept
{
    std::lock_guard lock{mutex_};
    return source && source.value() <= states_.size()
        ? states_[source.value() - 1].counts : source_frontend_counts{};
}
std::uint32_t source_frontend_generation::remaining_dependencies(
    source_id source) const noexcept
{
    std::lock_guard lock{mutex_};
    return source && source.value() <= states_.size()
        ? states_[source.value() - 1].remaining : 0;
}
bool source_frontend_generation::published(source_id source) const noexcept
{
    std::lock_guard lock{mutex_};
    return source && source.value() <= states_.size() &&
           states_[source.value() - 1].published;
}
const source_environment_storage* source_frontend_generation::interface(
    source_id source) const noexcept
{
    std::lock_guard lock{mutex_};
    return source && source.value() <= states_.size()
        ? states_[source.value() - 1].interface.get() : nullptr;
}
bool source_frontend_generation::failed() const noexcept
{
    std::lock_guard lock{mutex_};
    return !failure_.ok();
}
}
