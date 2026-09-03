#include "project_builder.hpp"

#include "../../diagnostics/diagnostic_descriptor.hpp"
#include "../graph/graph_build_transaction.hpp"

#include <new>
#include <vector>
#include <cstdint>

namespace cw::server
{

namespace
{
struct type_resolution_cache_entry
{
    string_id name{};
    TypeRef base{};
};
struct derived_cache_entry
{
    derived_type_kind kind{};
    TypeRef child{};
    std::uint64_t payload{};
    TypeRef resolved{};
};

struct type_resolution_context
{
    std::vector<type_resolution_cache_entry> names;
    std::vector<derived_cache_entry> derived;
    std::uint64_t lookups{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t probes{};
};

TypeRef find_named(const type_resolution_context& c, string_id name) noexcept
{
    for (const auto& e : c.names) if (e.name == name) return e.base;
    return {};
}

TypeRef find_derived(type_resolution_context& c, derived_type_kind kind,
                    TypeRef child, std::uint64_t payload) noexcept
{
    ++c.lookups;
    for (const auto& e : c.derived)
    {
        ++c.probes;
        if (e.kind == kind && e.child == child && e.payload == payload)
        { ++c.hits; return e.resolved; }
    }
    ++c.misses;
    return {};
}

status build_aggregate_impl(graph_update::source_replacement& replacement,
                            const aggregate_source_fact& fact,
                            type_resolution_context& cache) noexcept
{
    stable_id entity; type_handle type;
    auto result = replacement.add_named_type(fact.canonical_name, fact.definition_state, entity, type);
    if (!result.ok()) return result;
    if (!entity || !type) return {status_code::initialization_failed};
    if (fact.definition_state != aggregate_definition_state::defined)
        return fact.members.empty() ? status{} : status{status_code::configuration_failed};
    try {
        std::vector<member_build> members; members.reserve(fact.members.size());
        for (const auto& member : fact.members)
        {
            if (!member.name || (member.builtin.has_value() == static_cast<bool>(member.user_type_name)))
                return {status_code::configuration_failed};
            TypeRef base;
            if (member.builtin) base = replacement.builtin_type_ref(*member.builtin);
            else {
                base = find_named(cache, member.user_type_name);
                if (!base) { result = replacement.resolve_type(member.user_type_name, base); if (!result.ok()) return result; cache.names.push_back({member.user_type_name, base}); }
            }
            TypeRef resolved = base;
            if (member.lvalue_reference)
            {
                resolved = find_derived(cache, derived_type_kind::lvalue_reference, base, 0);
                if (!resolved) { result = replacement.get_or_create_lvalue_reference(base, resolved); if (!result.ok()) return result; cache.derived.push_back({derived_type_kind::lvalue_reference, base, 0, resolved}); }
            }
            members.push_back({member.name, resolved});
        }
        return replacement.define_members(type, members);
    } catch (...) { return {status_code::initialization_failed}; }
}
}

status project_builder::build(
    graph_build_transaction& transaction,
    const std::span<const source_fact_batch> sources,
    const operation_id operation,
    diagnostic_buffer& diagnostic_output) const noexcept
{
    const auto abort = [&transaction](const status result) noexcept
    {
        transaction.fail(result);
        return result;
    };

    try
    {
        // Validate fact shape before semantic mutation. Batch and declaration
        // positions are authoritative and are never reconstructed or sorted.
        for (const auto& batch : sources)
        {
            if (!batch.source)
                return abort({status_code::configuration_failed});

            for (const auto& fact : batch.enums)
                if (fact.anonymous == static_cast<bool>(fact.canonical_name))
                    return abort({status_code::configuration_failed});
            for (const auto& fact : batch.aggregates)
                if (!fact.canonical_name)
                    return abort({status_code::configuration_failed});
        }

        std::vector<enum_value_build> enumerators;
        type_resolution_context type_cache;

        for (const auto& batch : sources)
        {
            graph_update::source_replacement replacement;
            const auto opened = transaction.graph_state().replace_source(
                batch.source, replacement);
            if (!opened.ok())
            {
                if (opened.code == status_code::duplicate_source_replacement)
                {
                    try
                    {
                        diagnostic_output.emit({
                            diagnostics::builder_duplicate_source_replacement.id,
                            diagnostics::builder_duplicate_source_replacement.default_severity,
                            operation,
                            {batch.source, 0, 0},
                            {}});
                    }
                    catch (...)
                    {
                        return abort({status_code::initialization_failed});
                    }
                }
                return abort(opened);
            }

            for (const auto& fact : batch.enums)
            {
                enumerators.clear();
                enumerators.reserve(fact.enumerators.size());
                for (const auto& value : fact.enumerators)
                    enumerators.push_back({value.name, value.value});

                const enum_build_data data{
                    fact.definition_state,
                    fact.scoped,
                    fact.explicit_underlying,
                    enumerators};

                type_handle type{};
                status result{};
                if (fact.anonymous)
                {
                    result = replacement.add_anonymous_enum(data, type);
                }
                else
                {
                    stable_id entity{};
                    result = replacement.add_named_enum(
                        fact.canonical_name, data, entity, type);
                    if (result.ok() && (!entity || !type))
                        return abort({status_code::initialization_failed});
                }

                if (!result.ok())
                    return abort(result);
                if (!type)
                    return abort({status_code::initialization_failed});
            }
            for (const auto& fact : batch.aggregates)
            {
                const auto result = build_aggregate_impl(replacement, fact, type_cache);
                if (!result.ok()) return abort(result);
            }
        }

        return {};
    }
    catch (const std::bad_alloc&)
    {
        return abort({status_code::initialization_failed});
    }
    catch (...)
    {
        return abort({status_code::initialization_failed});
    }
}

status project_builder::build_enum(
    graph_update::source_replacement& replacement,
    const enum_source_fact& fact) const noexcept
{
    try
    {
        std::vector<enum_value_build> values;
        values.reserve(fact.enumerators.size());
        for (const auto& value : fact.enumerators)
            values.push_back({value.name, value.value});
        const enum_build_data data{fact.definition_state, fact.scoped,
                                   fact.explicit_underlying, values};
        type_handle type;
        status result;
        if (fact.anonymous)
            result = replacement.add_anonymous_enum(data, type);
        else
        {
            stable_id entity;
            result = replacement.add_named_enum(
                fact.canonical_name, data, entity, type);
            if (result.ok() && !entity) return {status_code::initialization_failed};
        }
        if (result.ok() && !type) return {status_code::initialization_failed};
        return result;
    }
    catch (...)
    {
        return {status_code::initialization_failed};
    }
}

status project_builder::build_aggregate(
    graph_update::source_replacement& replacement,
    const aggregate_source_fact& fact) const noexcept
{
    type_resolution_context cache;
    return build_aggregate_impl(replacement, fact, cache);
}

} // namespace cw::server
