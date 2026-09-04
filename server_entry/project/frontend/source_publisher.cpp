#include "source_publisher.hpp"

#include "../builder/project_builder.hpp"
#include "../graph/graph_build_transaction.hpp"
#include "../parser/source_context.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <vector>

namespace cw::server {
namespace {

status copy_name(
    const source_context& context,
    source_name_ref source,
    source_build_entry& destination,
    build_name_ref& output) noexcept {

    output = {};

    if (!source) {
        return {};
    }

    std::string_view bytes;
    auto result = context.resolve_name(source, bytes);

    if (!result.ok()) {
        return result;
    }

    return destination.store_name(bytes, output);
}

} // namespace

status capture_source_facts(
    const parser_source_fact_batch& batch,
    source_build_entry& output) noexcept {

    output.reset();

    if (!batch.source || !batch.context) {
        return {status_code::configuration_failed};
    }

    try {
        output.source = batch.source;

        output.enums.reserve(batch.enums.size());
        output.aggregates.reserve(batch.aggregates.size());

        for (const auto& fact : batch.enums) {
            if (fact.anonymous == static_cast<bool>(fact.canonical_name)) {
                return {status_code::configuration_failed};
            }

            source_build_enum captured;
            captured.anonymous = fact.anonymous;
            captured.scoped = fact.scoped;
            captured.definition_state = fact.definition_state;
            captured.explicit_underlying = fact.explicit_underlying;
            captured.declaration_range = fact.declaration_range;
            captured.name_range = fact.name_range;

            auto result = copy_name(
                *batch.context,
                fact.canonical_name,
                output,
                captured.canonical_name);

            if (!result.ok()) {
                return result;
            }

            const auto values = batch.context->enumerators(fact);

            if (values.size() != fact.enumerator_count) {
                return {status_code::configuration_failed};
            }

            captured.value_offset = static_cast<std::uint32_t>(output.enum_values.size());
            captured.value_count = static_cast<std::uint32_t>(values.size());

            for (const auto& value : values) {
                source_build_enum_value captured_value;
                captured_value.value = value.value;
                captured_value.name_range = value.name_range;

                result = copy_name(
                    *batch.context,
                    value.name,
                    output,
                    captured_value.name);

                if (!result.ok() || !captured_value.name) {
                    return result.ok()
                        ? status{status_code::configuration_failed}
                        : result;
                }

                output.enum_values.push_back(captured_value);
            }

            output.enums.push_back(captured);
        }

        for (const auto& fact : batch.aggregates) {
            source_build_aggregate captured;
            captured.definition_state = fact.definition_state;
            captured.declaration_range = fact.declaration_range;
            captured.name_range = fact.name_range;

            auto result = copy_name(
                *batch.context,
                fact.canonical_name,
                output,
                captured.canonical_name);

            if (!result.ok() || !captured.canonical_name) {
                return result.ok()
                    ? status{status_code::configuration_failed}
                    : result;
            }

            const auto source_members = batch.context->members(fact);

            if (source_members.size() != fact.member_count) {
                return {status_code::configuration_failed};
            }

            captured.member_offset = static_cast<std::uint32_t>(output.members.size());
            captured.member_count = static_cast<std::uint32_t>(source_members.size());

            for (const auto& member : source_members) {
                source_build_member captured_member;
                captured_member.builtin = member.builtin;

                result = copy_name(
                    *batch.context,
                    member.name,
                    output,
                    captured_member.name);

                if (!result.ok() || !captured_member.name) {
                    return result.ok()
                        ? status{status_code::configuration_failed}
                        : result;
                }

                if (!member.builtin) {
                    result = copy_name(
                        *batch.context,
                        member.type_name,
                        output,
                        captured_member.user_type_name);

                    if (!result.ok() || !captured_member.user_type_name) {
                        return result.ok()
                            ? status{status_code::configuration_failed}
                            : result;
                    }
                }

                const auto source_modifiers = batch.context->modifiers(member);

                if (source_modifiers.size() != member.modifier_count) {
                    return {status_code::configuration_failed};
                }

                captured_member.modifier_offset =
                    static_cast<std::uint32_t>(output.modifiers.size());
                captured_member.modifier_count =
                    static_cast<std::uint32_t>(source_modifiers.size());

                for (const auto& modifier : source_modifiers) {
                    derived_type_kind kind;

                    switch (modifier.kind) {
                    case source_type_modifier_kind::pointer:
                        kind = derived_type_kind::pointer;
                        break;
                    case source_type_modifier_kind::array:
                        kind = derived_type_kind::array;
                        break;
                    case source_type_modifier_kind::lvalue_reference:
                        kind = derived_type_kind::lvalue_reference;
                        break;
                    case source_type_modifier_kind::rvalue_reference:
                        kind = derived_type_kind::rvalue_reference;
                        break;
                    default:
                        return {status_code::configuration_failed};
                    }

                    output.modifiers.push_back({kind, modifier.payload});
                }

                output.members.push_back(captured_member);
            }

            output.aggregates.push_back(captured);
        }

        return {};
    }
    catch (...) {
        output.reset();
        return {status_code::initialization_failed};
    }
}

status publish_source_entry(
    graph_build_transaction& transaction,
    const source_build_entry& entry,
    const project_builder& builder,
    const operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    const auto abort = [&](status result) noexcept {
        transaction.fail(result);
        return result;
    };

    const auto emit = [&](
        const diagnostic_descriptor& descriptor,
        source_range location) noexcept {
        try {
            diagnostics.emit({
                descriptor.id,
                descriptor.default_severity,
                operation,
                location,
                {}
            });
            return status{};
        }
        catch (...) {
            return status{status_code::initialization_failed};
        }
    };

    const auto malformed = [&]() noexcept {
        const auto emitted = emit(
            diagnostics::builder_invalid_source_fact,
            {entry.source, 0, 0});
        return abort(emitted.ok()
            ? status{status_code::configuration_failed}
            : emitted);
    };

    const auto infrastructure = [&]() noexcept {
        const auto emitted = emit(
            diagnostics::construction_initialization_failed,
            {entry.source, 0, 0});
        return abort(emitted.ok()
            ? status{status_code::initialization_failed}
            : emitted);
    };

    if (!entry.source) {
        return malformed();
    }

    try {
        graph_update::source_replacement replacement;
        auto result = transaction.graph_state().replace_source(entry.source, replacement);

        if (!result.ok()) {
            if (result.code == status_code::duplicate_source_replacement) {
                const auto emitted = emit(
                    diagnostics::builder_duplicate_source_replacement,
                    {entry.source, 0, 0});
                return abort(emitted.ok() ? result : emitted);
            }

            return result.code == status_code::initialization_failed
                ? infrastructure()
                : abort(result);
        }

        std::vector<enum_value_fact> enum_values;
        std::vector<aggregate_source_fact::member_fact> members;
        std::vector<canonical_type_modifier> modifiers;

        for (const auto& fact : entry.enums) {
            string_id canonical_name;

            if (!fact.anonymous) {
                std::string_view bytes;
                result = entry.resolve_name(fact.canonical_name, bytes);
                if (!result.ok()) {
                    return malformed();
                }

                result = transaction.strings().intern(bytes, canonical_name);
                if (!result.ok()) {
                    return result.code == status_code::initialization_failed
                        ? infrastructure()
                        : abort(result);
                }
            }

            if (fact.value_offset > entry.enum_values.size() ||
                fact.value_count > entry.enum_values.size() - fact.value_offset) {
                return malformed();
            }

            enum_values.clear();
            enum_values.reserve(fact.value_count);

            for (std::uint32_t index = 0; index < fact.value_count; ++index) {
                const auto& value = entry.enum_values[fact.value_offset + index];
                std::string_view bytes;
                result = entry.resolve_name(value.name, bytes);
                if (!result.ok()) {
                    return malformed();
                }

                string_id name;
                result = transaction.strings().intern(bytes, name);
                if (!result.ok() || !name) {
                    return result.code == status_code::initialization_failed
                        ? infrastructure()
                        : abort(result.ok()
                              ? status{status_code::configuration_failed}
                              : result);
                }

                enum_values.push_back({name, value.value});
            }

            const enum_source_fact canonical{
                canonical_name,
                fact.anonymous,
                fact.scoped,
                fact.definition_state,
                fact.explicit_underlying,
                enum_values
            };

            result = builder.build_enum(replacement, canonical);
            if (!result.ok()) {
                if (result.code == status_code::configuration_failed) {
                    const auto emitted = emit(
                        diagnostics::builder_semantic_failure,
                        {entry.source,
                         fact.declaration_range.offset,
                         fact.declaration_range.length});
                    return abort(emitted.ok() ? result : emitted);
                }
                return result.code == status_code::initialization_failed
                    ? infrastructure()
                    : abort(result);
            }
        }

        for (const auto& fact : entry.aggregates) {
            std::string_view bytes;
            result = entry.resolve_name(fact.canonical_name, bytes);
            if (!result.ok()) {
                return malformed();
            }

            string_id canonical_name;
            result = transaction.strings().intern(bytes, canonical_name);
            if (!result.ok() || !canonical_name) {
                return result.code == status_code::initialization_failed
                    ? infrastructure()
                    : abort(result.ok()
                          ? status{status_code::configuration_failed}
                          : result);
            }

            if (fact.member_offset > entry.members.size() ||
                fact.member_count > entry.members.size() - fact.member_offset) {
                return malformed();
            }

            members.clear();
            modifiers.clear();
            members.reserve(fact.member_count);

            for (std::uint32_t index = 0; index < fact.member_count; ++index) {
                const auto& member = entry.members[fact.member_offset + index];
                std::string_view member_bytes;
                result = entry.resolve_name(member.name, member_bytes);
                if (!result.ok()) {
                    return malformed();
                }

                string_id member_name;
                result = transaction.strings().intern(member_bytes, member_name);
                if (!result.ok() || !member_name) {
                    return result.code == status_code::initialization_failed
                        ? infrastructure()
                        : abort(result.ok()
                              ? status{status_code::configuration_failed}
                              : result);
                }

                string_id user_type_name;
                if (!member.builtin) {
                    std::string_view type_bytes;
                    result = entry.resolve_name(member.user_type_name, type_bytes);
                    if (!result.ok()) {
                        return malformed();
                    }
                    result = transaction.strings().intern(type_bytes, user_type_name);
                    if (!result.ok() || !user_type_name) {
                        return result.code == status_code::initialization_failed
                            ? infrastructure()
                            : abort(result.ok()
                                  ? status{status_code::configuration_failed}
                                  : result);
                    }
                }

                if (member.modifier_offset > entry.modifiers.size() ||
                    member.modifier_count > entry.modifiers.size() - member.modifier_offset) {
                    return malformed();
                }

                const auto modifier_offset =
                    static_cast<std::uint32_t>(modifiers.size());

                for (std::uint32_t modifier_index = 0;
                     modifier_index < member.modifier_count;
                     ++modifier_index) {
                    const auto& modifier =
                        entry.modifiers[member.modifier_offset + modifier_index];
                    modifiers.push_back({modifier.kind, modifier.payload});
                }

                members.push_back({
                    member_name,
                    member.builtin,
                    user_type_name,
                    modifier_offset,
                    member.modifier_count
                });
            }

            result = builder.build_aggregate(
                replacement,
                {
                    canonical_name,
                    fact.definition_state,
                    members,
                    modifiers
                });

            if (!result.ok()) {
                if (result.code == status_code::configuration_failed) {
                    const auto emitted = emit(
                        diagnostics::builder_semantic_failure,
                        {entry.source,
                         fact.declaration_range.offset,
                         fact.declaration_range.length});
                    return abort(emitted.ok() ? result : emitted);
                }
                return result.code == status_code::initialization_failed
                    ? infrastructure()
                    : abort(result);
            }
        }

        return {};
    }
    catch (...) {
        return infrastructure();
    }
}

status publish_source_facts(
    graph_build_transaction& transaction,
    const parser_source_fact_batch& batch,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    source_build_entry entry;
    auto result = capture_source_facts(batch, entry);

    if (!result.ok()) {
        transaction.fail(result);
        return result;
    }

    return publish_source_entry(
        transaction,
        entry,
        builder,
        operation,
        diagnostics);
}

} // namespace cw::server
