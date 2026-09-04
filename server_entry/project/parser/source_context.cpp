#include "source_context.hpp"

#include <limits>

namespace cw::server {

status source_context::store_name(
    std::string_view value,
    source_name_ref& result) noexcept {

    result = {};

    if (value.empty()) {
        return {status_code::configuration_failed};
    }

    const auto maximum =
        (std::numeric_limits<std::uint32_t>::max)();

    if (value.size() > maximum ||
        names.size() > maximum - value.size()) {
        return {status_code::initialization_failed};
    }

    try {
        result = {
            static_cast<std::uint32_t>(names.size()),
            static_cast<std::uint32_t>(value.size())
        };

        names.insert(
            names.end(),
            value.begin(),
            value.end());

        return {};
    }
    catch (...) {
        result = {};
        return {status_code::initialization_failed};
    }
}

status source_context::resolve_name(
    source_name_ref reference,
    std::string_view& output) const noexcept {

    output = {};

    const auto end =
        std::uint64_t{reference.offset} +
        reference.length;

    if (!reference || end > names.size()) {
        return {status_code::configuration_failed};
    }

    output = {
        names.data() + reference.offset,
        reference.length
    };

    return {};
}

std::span<const enum_value_source_fact> source_context::enumerators(
    const enum_declaration_source_fact& declaration) const noexcept {

    if (declaration.enumerator_count == 0) {
        return {};
    }

    const auto end =
        std::uint64_t{declaration.enumerator_offset} +
        declaration.enumerator_count;

    if (end > enum_values.size()) {
        return {};
    }

    return {
        enum_values.data() + declaration.enumerator_offset,
        declaration.enumerator_count
    };
}

std::span<const member_declaration_source_fact> source_context::members(
    const aggregate_declaration_source_fact& declaration) const noexcept {

    if (declaration.member_count == 0) {
        return {};
    }

    const auto end =
        std::uint64_t{declaration.member_offset} +
        declaration.member_count;

    if (end > aggregate_members.size()) {
        return {};
    }

    return {
        aggregate_members.data() + declaration.member_offset,
        declaration.member_count
    };
}

std::span<const source_type_modifier> source_context::modifiers(
    const member_declaration_source_fact& member) const noexcept {

    if (member.modifier_count == 0) {
        return {};
    }

    const auto end =
        std::uint64_t{member.modifier_offset} +
        member.modifier_count;

    if (end > type_modifiers.size()) {
        return {};
    }

    return {
        type_modifiers.data() + member.modifier_offset,
        member.modifier_count
    };
}

void source_context::reset() noexcept {
    names.clear();
    tokens.clear();
    enum_values.clear();
    enums.clear();
    aggregates.clear();
    aggregate_members.clear();
    type_modifiers.clear();
    diagnostics.clear();
}

} // namespace cw::server
