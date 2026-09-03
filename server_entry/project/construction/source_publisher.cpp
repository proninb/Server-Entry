#include "source_publisher.hpp"

#include "../builder/project_builder.hpp"
#include "../graph/graph_build_transaction.hpp"
#include "../parser/source_context.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <vector>

namespace cw::server
{
status publish_source_facts(
    graph_build_transaction& transaction,
    const parser_source_fact_batch& batch,
    const project_builder& builder,
    const operation_id operation,
    diagnostic_buffer& diagnostics) noexcept
{
    const auto abort = [&](status result) noexcept
    {
        transaction.fail(result);
        return result;
    };
    const auto emit = [&](const diagnostic_descriptor& descriptor,
                          source_range location) noexcept
    {
        try
        {
            diagnostics.emit({descriptor.id, descriptor.default_severity,
                operation, location, {}});
            return status{};
        }
        catch (...)
        {
            return status{status_code::initialization_failed};
        }
    };
    const auto malformed = [&](source_id source) noexcept
    {
        const auto emitted = emit(
            diagnostics::builder_invalid_source_fact, {source, 0, 0});
        return abort(emitted.ok()
            ? status{status_code::configuration_failed} : emitted);
    };
    const auto infrastructure = [&](source_id source) noexcept
    {
        const auto emitted = emit(
            diagnostics::construction_initialization_failed, {source, 0, 0});
        return abort(emitted.ok()
            ? status{status_code::initialization_failed} : emitted);
    };

    try
    {
        std::vector<enum_value_fact> canonical_values;
        std::vector<aggregate_source_fact::member_fact> canonical_members;
        {
            if (!batch.source || !batch.context) return malformed(batch.source);
            graph_update::source_replacement replacement;
            const auto opened = transaction.graph_state().replace_source(
                batch.source, replacement);
            if (!opened.ok())
            {
                if (opened.code == status_code::duplicate_source_replacement)
                {
                    const auto emitted = emit(
                        diagnostics::builder_duplicate_source_replacement,
                        {batch.source, 0, 0});
                    return abort(emitted.ok() ? opened : emitted);
                }
                return opened.code == status_code::initialization_failed
                    ? infrastructure(batch.source) : abort(opened);
            }

            for (const auto& fact : batch.enums)
            {
                if (fact.anonymous == static_cast<bool>(fact.canonical_name))
                    return malformed(batch.source);
                string_id canonical_name;
                if (!fact.anonymous)
                {
                    std::string_view bytes;
                    auto result = batch.context->resolve_name(
                        fact.canonical_name, bytes);
                    if (!result.ok()) return malformed(batch.source);
                    result = transaction.strings().intern(bytes, canonical_name);
                    if (!result.ok())
                    {
                        if (result.code == status_code::configuration_failed)
                        {
                            const auto emitted = emit(
                                diagnostics::canonicalization_failed,
                                {batch.source, fact.name_range.offset,
                                 fact.name_range.length});
                            return abort(emitted.ok() ? result : emitted);
                        }
                        return result.code == status_code::initialization_failed
                            ? infrastructure(batch.source) : abort(result);
                    }
                    if (!canonical_name) return infrastructure(batch.source);
                }

                const auto source_values = batch.context->enumerators(fact);
                if (source_values.size() != fact.enumerator_count)
                    return malformed(batch.source);
                canonical_values.clear();
                canonical_values.reserve(source_values.size());
                for (const auto& value : source_values)
                {
                    std::string_view bytes;
                    auto result = batch.context->resolve_name(value.name, bytes);
                    if (!result.ok()) return malformed(batch.source);
                    string_id name;
                    result = transaction.strings().intern(bytes, name);
                    if (!result.ok())
                    {
                        if (result.code == status_code::configuration_failed)
                        {
                            const auto emitted = emit(
                                diagnostics::canonicalization_failed,
                                {batch.source, value.name_range.offset,
                                 value.name_range.length});
                            return abort(emitted.ok() ? result : emitted);
                        }
                        return result.code == status_code::initialization_failed
                            ? infrastructure(batch.source) : abort(result);
                    }
                    if (!name) return infrastructure(batch.source);
                    canonical_values.push_back({name, value.value});
                }

                const enum_source_fact canonical_fact{
                    canonical_name, fact.anonymous, fact.scoped,
                    fact.definition_state, fact.explicit_underlying,
                    canonical_values};
                const auto result = builder.build_enum(replacement, canonical_fact);
                if (!result.ok())
                {
                    if (result.code == status_code::configuration_failed)
                    {
                        const auto emitted = emit(
                            diagnostics::builder_semantic_failure,
                            {batch.source, fact.declaration_range.offset,
                             fact.declaration_range.length});
                        return abort(emitted.ok() ? result : emitted);
                    }
                    return result.code == status_code::initialization_failed
                        ? infrastructure(batch.source) : abort(result);
                }
            }
            for (const auto& fact : batch.aggregates)
            {
                std::string_view bytes;
                auto result = batch.context->resolve_name(fact.canonical_name, bytes);
                if (!result.ok()) return malformed(batch.source);
                string_id canonical_name;
                result = transaction.strings().intern(bytes, canonical_name);
                if (!result.ok())
                {
                    if (result.code == status_code::configuration_failed)
                    {
                        const auto emitted = emit(
                            diagnostics::canonicalization_failed,
                            {batch.source, fact.name_range.offset,
                             fact.name_range.length});
                        return abort(emitted.ok() ? result : emitted);
                    }
                    return result.code == status_code::initialization_failed
                        ? infrastructure(batch.source) : abort(result);
                }
                canonical_members.clear();
                const auto source_members = batch.context->members(fact);
                if (source_members.size() != fact.member_count)
                    return malformed(batch.source);
                canonical_members.reserve(source_members.size());
                for (const auto& member : source_members)
                {
                    std::string_view member_bytes;
                    result = batch.context->resolve_name(member.name, member_bytes);
                    if (!result.ok()) return malformed(batch.source);
                    string_id member_name;
                    result = transaction.strings().intern(member_bytes, member_name);
                    if (!result.ok()) return result.code == status_code::initialization_failed
                        ? infrastructure(batch.source) : abort(result);
                    string_id user_type_name;
                    if (!member.builtin)
                    {
                        std::string_view type_bytes;
                        result = batch.context->resolve_name(member.type_name, type_bytes);
                        if (!result.ok()) return malformed(batch.source);
                        result = transaction.strings().intern(type_bytes, user_type_name);
                        if (!result.ok()) return result.code == status_code::initialization_failed
                            ? infrastructure(batch.source) : abort(result);
                    }
                    canonical_members.push_back({member_name, member.builtin,
                        user_type_name, member.lvalue_reference});
                }
                result = builder.build_aggregate(replacement,
                    {canonical_name, fact.definition_state, canonical_members});
                if (!result.ok())
                {
                    if (result.code == status_code::configuration_failed)
                    {
                        const auto emitted = emit(
                            diagnostics::builder_semantic_failure,
                            {batch.source, fact.declaration_range.offset,
                             fact.declaration_range.length});
                        return abort(emitted.ok() ? result : emitted);
                    }
                    return result.code == status_code::initialization_failed
                        ? infrastructure(batch.source) : abort(result);
                }
            }
        }
        return {};
    }
    catch (...)
    {
        return infrastructure(batch.source);
    }
}
}
