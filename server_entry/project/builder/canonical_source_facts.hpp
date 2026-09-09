#pragma once

#include "../../member_index.hpp"
#include "../../source_id.hpp"
#include "../../string_id.hpp"
#include "../source_entity_ref.hpp"
#include "../graph/enum_build.hpp"
#include "../graph/type_ref.hpp"
#include "../language/aggregate_semantics.hpp"
#include "../language/enum_semantics.hpp"

#include <optional>
#include <span>

namespace cw::server {

using enum_value_fact = enum_value_build;

struct enum_source_fact {
    string_id canonical_name{};
    bool anonymous = false;
    bool scoped = false;

    enum_definition_state definition_state =
        enum_definition_state::defined;

    std::optional<builtin_type> explicit_underlying;
    std::span<const enum_value_fact> enumerators;

    source_entity_ref source_entity{};
};

struct canonical_type_modifier {
    derived_type_kind kind = derived_type_kind::pointer;
    std::uint64_t payload = 0;
};

struct aggregate_source_fact {
    struct member_fact {
        string_id name{};
        std::optional<builtin_type> builtin;

        // Compatibility canonical-name path for non-Parser producers.
        string_id user_type_name{};

        std::uint32_t modifier_offset = 0;
        std::uint32_t modifier_count = 0;

        // Production relation. No text lookup occurs after this exists.
        source_entity_ref user_type_entity{};
    };

    string_id canonical_name{};

    aggregate_definition_state definition_state =
        aggregate_definition_state::declared;

    std::span<const member_fact> members{};
    std::span<const canonical_type_modifier> modifiers{};

    source_entity_ref source_entity{};
};

struct canonical_static_object_fact {
    std::optional<builtin_type> builtin;
    source_entity_ref user_type_entity{};
    std::span<const canonical_type_modifier> modifiers{};
};

struct canonical_construction_binding_fact {
    source_entity_ref owner_type{};
    member_index member{};
    std::uint32_t static_object = 0;
};

struct source_fact_batch {
    source_id source{};
    std::span<const enum_source_fact> enums;
    std::span<const aggregate_source_fact> aggregates;
};

} // namespace cw::server