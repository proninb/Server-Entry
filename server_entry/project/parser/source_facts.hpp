#pragma once

#include "../../source_id.hpp"
#include "../graph/builtin_type.hpp"
#include "../language/aggregate_semantics.hpp"
#include "../language/enum_semantics.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace cw::server {

// Identifies a half-open byte range inside the immutable Source snapshot that
// produced these facts. Ranges are source-local diagnostics/provenance data and
// do not identify canonical Graph entities.
struct source_text_range {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

// References one spelling stored in the owning source_context name arena.
// The reference is compact and non-owning; it is valid only while that context
// retains the corresponding parse result.
struct source_name_ref {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return length != 0;
    }
};

// Describes one enumerator exactly as interpreted by Parser for one Source.
// The value contains source-language constant semantics; Builder later maps the
// enclosing declaration into canonical Graph identity.
struct enum_value_source_fact {
    source_name_ref name{};
    integral_constant value{};
    source_text_range name_range{};
    source_text_range expression_range{};
};

// Describes one enum declaration or definition produced by Parser.
// canonical_name and scope_name are resolved source-language spellings stored in
// source_context; they are not stable_id values or Graph-owned identities.
struct enum_declaration_source_fact {
    source_name_ref canonical_name{};
    source_name_ref scope_name{};

    bool anonymous = false;
    bool scoped = false;

    enum_definition_state definition_state =
        enum_definition_state::defined;

    std::optional<builtin_type> explicit_underlying;

    // Enumerators occupy one contiguous slice of source_context::enum_values.
    std::uint32_t enumerator_offset = 0;
    std::uint32_t enumerator_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
    source_text_range underlying_range{};
};

// Describes one aggregate declaration or definition produced by Parser.
// Member records are stored separately in source_context and referenced as one
// contiguous source-local slice.
struct aggregate_declaration_source_fact {
    source_name_ref canonical_name{};
    source_name_ref scope_name{};

    aggregate_definition_state definition_state =
        aggregate_definition_state::declared;

    std::uint32_t member_offset = 0;
    std::uint32_t member_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
};

// Describes one source-language modifier applied around a member base type.
// Modifiers are stored in base-to-outer order and remain source semantics only.
enum class source_type_modifier_kind : std::uint8_t {
    pointer,
    array,
    lvalue_reference,
    rvalue_reference
};

struct source_type_modifier {
    source_type_modifier_kind kind = source_type_modifier_kind::pointer;
    std::uint64_t payload = 0;
    source_text_range range{};
};

// Describes one non-static instance member in the source-language model.
// A member type is represented either as an intrinsic builtin or as a resolved
// source-language type name; canonical type identity and layout are Builder work.
struct member_declaration_source_fact {
    source_name_ref name{};
    source_name_ref type_name{};

    std::optional<builtin_type> builtin;

    // Modifiers occupy one contiguous slice of source_context::type_modifiers.
    std::uint32_t modifier_offset = 0;
    std::uint32_t modifier_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
    source_text_range type_range{};
};

class source_context;

// Presents the transient Parser result for one Source to the next pipeline stage.
// The batch owns nothing: context and both spans must remain valid for the entire
// synchronous consumption of the batch. Builder must not retain these references.
struct parser_source_fact_batch {
    source_id source{};
    const source_context* context = nullptr;

    std::span<const enum_declaration_source_fact> enums;
    std::span<const aggregate_declaration_source_fact> aggregates;
};

} // namespace cw::server
