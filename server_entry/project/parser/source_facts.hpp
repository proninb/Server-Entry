#pragma once

#include "../source_entity_ref.hpp"
#include "../graph/builtin_type.hpp"
#include "../language/aggregate_semantics.hpp"
#include "../language/enum_semantics.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace cw::server {

// Identifies a half-open byte range inside the immutable Source snapshot that
// produced these facts. Ranges are diagnostics/provenance, not Entity identity.
struct source_text_range {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

// References one spelling in the owning source_context arena.
// Semantic relations use source_entity_ref after Parser resolution.
struct source_name_ref {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return length != 0;
    }
};

// Dense Parser-context record for one named type declaration.
struct source_type_declaration {
    source_entity_ref entity{};
    source_name_ref canonical_name{};
    source_text_range name_range{};
};

struct enum_value_source_fact {
    source_name_ref name{};
    integral_constant value{};
    source_text_range name_range{};
    source_text_range expression_range{};
};

struct enum_declaration_source_fact {
    source_entity_ref entity{};
    source_name_ref canonical_name{};
    source_name_ref scope_name{};

    bool anonymous = false;
    bool scoped = false;

    enum_definition_state definition_state =
        enum_definition_state::defined;

    std::optional<builtin_type> explicit_underlying;

    std::uint32_t enumerator_offset = 0;
    std::uint32_t enumerator_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
    source_text_range underlying_range{};
};

struct aggregate_declaration_source_fact {
    source_entity_ref entity{};
    source_name_ref canonical_name{};
    source_name_ref scope_name{};

    aggregate_definition_state definition_state =
        aggregate_definition_state::declared;

    std::uint32_t member_offset = 0;
    std::uint32_t member_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
};

enum class source_type_modifier_kind : std::uint8_t {
    pointer,
    array,
    lvalue_reference,
    rvalue_reference
};

struct source_type_modifier {
    source_type_modifier_kind kind =
        source_type_modifier_kind::pointer;

    std::uint64_t payload = 0;
    source_text_range range{};
};

// type_entity is authoritative after successful user-type resolution.
// type_name is retained only for diagnostics/source-interface provenance.
struct member_declaration_source_fact {
    source_name_ref name{};
    source_name_ref type_name{};

    std::optional<builtin_type> builtin;

    std::uint32_t modifier_offset = 0;
    std::uint32_t modifier_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
    source_text_range type_range{};

    source_entity_ref type_entity{};
};

class source_context;

struct parser_source_fact_batch {
    source_id source{};
    const source_context* context = nullptr;

    std::span<const enum_declaration_source_fact> enums;
    std::span<const aggregate_declaration_source_fact> aggregates;
};

} // namespace cw::server