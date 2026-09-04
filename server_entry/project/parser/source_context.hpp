#pragma once

#include "source_facts.hpp"
#include "token.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

// Owns the reusable transient result of parsing one Source.
// source_context stores tokens, source-language facts, compact name text, and
// diagnostics for the current parse. It is Parser output only: Builder may
// consume these facts synchronously, but the context is not persistent Graph state.
class source_context final {
public:
    [[nodiscard]] status store_name(
        std::string_view value,
        source_name_ref& result) noexcept;

    [[nodiscard]] status resolve_name(
        source_name_ref reference,
        std::string_view& output) const noexcept;

    [[nodiscard]] std::span<const enum_value_source_fact> enumerators(
        const enum_declaration_source_fact& declaration) const noexcept;

    [[nodiscard]] std::span<const member_declaration_source_fact> members(
        const aggregate_declaration_source_fact& declaration) const noexcept;

    [[nodiscard]] std::span<const source_type_modifier> modifiers(
        const member_declaration_source_fact& member) const noexcept;

    // Clears all state so the same context can be reused for another Source.
    void reset() noexcept;

    std::vector<parser_token> tokens;
    std::vector<enum_value_source_fact> enum_values;
    std::vector<enum_declaration_source_fact> enums;
    std::vector<aggregate_declaration_source_fact> aggregates;
    std::vector<member_declaration_source_fact> aggregate_members;
    std::vector<source_type_modifier> type_modifiers;
    diagnostic_buffer diagnostics;

private:
    // Name references are offsets into this packed byte storage rather than
    // owning strings, keeping source facts compact and cheap to move.
    std::vector<char> names;
};

} // namespace cw::server
