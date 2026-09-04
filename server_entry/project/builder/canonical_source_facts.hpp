#pragma once

#include "../../source_id.hpp"
#include "../../string_id.hpp"
#include "../graph/builtin_type.hpp"
#include "../graph/type_ref.hpp"
#include "../language/aggregate_semantics.hpp"
#include "../language/enum_semantics.hpp"

#include <optional>
#include <span>

namespace cw::server {

// Builder-ready representation of one enum value.
// name is already interned in String Registry; value retains the interpreted
// source-language integral constant until Graph applies target-ABI semantics.
struct enum_value_fact {
    string_id name{};
    integral_constant value{};
};

// Builder-ready representation of one enum declaration or definition.
// canonical_name is already the resolved/interned source-language name.
// Anonymous enums deliberately carry no canonical_name.
//
// enumerators is non-owning and must remain valid for the synchronous Builder
// call that consumes this fact.
struct enum_source_fact {
    string_id canonical_name{};
    bool anonymous = false;
    bool scoped = false;

    enum_definition_state definition_state =
        enum_definition_state::defined;

    std::optional<builtin_type> explicit_underlying;
    std::span<const enum_value_fact> enumerators;
};

// Canonical modifier request consumed by Graph TypeRef construction.
// Ordering is base-to-outer and is preserved exactly from Parser A'.
struct canonical_type_modifier {
    derived_type_kind kind = derived_type_kind::pointer;
    std::uint64_t payload = 0;
};

// Builder-ready representation of one aggregate declaration or definition.
// Source-language lookup is complete before this structure is produced;
// project_builder maps these canonical names into Graph identity and TypeRef.
struct aggregate_source_fact {
    // Builder-ready representation of one non-static instance member.
    // Exactly one base-type form is expected: builtin or user_type_name.
    // user_type_name is already a resolved canonical source-language name.
    struct member_fact {
        string_id name{};
        std::optional<builtin_type> builtin;
        string_id user_type_name{};
        std::uint32_t modifier_offset = 0;
        std::uint32_t modifier_count = 0;
    };

    string_id canonical_name{};

    aggregate_definition_state definition_state =
        aggregate_definition_state::declared;

    // Non-owning synchronous Builder input.
    std::span<const member_fact> members{};
    std::span<const canonical_type_modifier> modifiers{};
};

// Groups all Builder-ready canonical facts contributed by one Source.
// Fact order is authoritative and is established before project_builder runs.
// The batch owns no referenced declaration/member/enumerator storage.
struct source_fact_batch {
    source_id source{};
    std::span<const enum_source_fact> enums;
    std::span<const aggregate_source_fact> aggregates;
};

} // namespace cw::server
