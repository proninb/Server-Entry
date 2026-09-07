#pragma once

#include "source_facts.hpp"
#include "token.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

// Reusable Source-local construction workspace. Parser owns lexical resolution,
// declaration uniqueness and diagnostics here, then transfers the completed
// semantic product by vector ownership move into source_facts_storage.
class source_context final {
public:
    [[nodiscard]] status store_name(
        std::string_view value,
        source_name_ref& result) noexcept;

    [[nodiscard]] status store_qualified_name(
        std::string_view scope,
        std::string_view local,
        source_name_ref& result) noexcept;

    [[nodiscard]] status resolve_name(
        source_name_ref reference,
        std::string_view& output) const noexcept;

    [[nodiscard]] status declare_type(
        source_id source,
        source_name_ref canonical_name,
        source_text_range name_range,
        source_entity_ref& result) noexcept;

    [[nodiscard]] status find_type(
        std::string_view scope,
        std::string_view name,
        source_entity_ref& entity,
        source_name_ref& canonical_name) noexcept;

    [[nodiscard]] status declare_constant(
        source_name_ref scope,
        source_name_ref name,
        integral_constant value) noexcept;

    // Transfers only the semantic product. Type/constant construction indexes,
    // tokens and diagnostics remain SourceContext-local and are reset afterward.
    [[nodiscard]] status release_facts(
        source_id source,
        source_facts_storage& output) noexcept;

    [[nodiscard]] std::span<const enum_value_source_fact> enumerators(
        const enum_declaration_source_fact& declaration) const noexcept;

    [[nodiscard]] std::span<const member_declaration_source_fact> members(
        const aggregate_declaration_source_fact& declaration) const noexcept;

    [[nodiscard]] std::span<const source_type_modifier> modifiers(
        const member_declaration_source_fact& member) const noexcept;

    void reset() noexcept;

    std::vector<parser_token> tokens;
    std::vector<source_type_declaration> type_declarations;
    std::vector<enum_value_source_fact> enum_values;
    std::vector<enum_declaration_source_fact> enums;
    std::vector<aggregate_declaration_source_fact> aggregates;
    std::vector<member_declaration_source_fact> aggregate_members;
    std::vector<source_type_modifier> type_modifiers;
    diagnostic_buffer diagnostics;

private:
    struct constant_symbol {
        source_name_ref scope{};
        source_name_ref name{};
        integral_constant value{};
    };

    [[nodiscard]] status ensure_type_index(
        std::size_t required) noexcept;

    [[nodiscard]] status ensure_constant_index(
        std::size_t required) noexcept;

    std::vector<char> names;
    std::uint32_t stored_name_count = 0;

    // Reference-demand index: declaration-only Sources never build it.
    std::vector<std::uint32_t> type_index;
    bool type_index_active = false;

    // Declaration-uniqueness index, not a reference lookup cache. It is required
    // while declarations are accepted even when no later constant is referenced.
    std::vector<constant_symbol> constant_symbols;
    std::vector<std::uint32_t> constant_index;
};

} // namespace cw::server
