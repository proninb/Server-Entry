#pragma once

#include "graph.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cw::server {

// Persistence DTO for one stable_id-indexed Entity entry. The stable_id is
// implicit in the vector position; liveness is likewise derived from name != 0,
// so neither identity nor a duplicate live flag is serialized.
struct compiled_entity_slot {
    entity_kind kind = entity_kind::aggregate_type;
    std::uint32_t name = 0;
    std::uint32_t type = 0;

    [[nodiscard]] constexpr bool live() const noexcept { return name != 0; }
};

// Persistence DTO for one materialized enum value.
struct compiled_enum_value {
    std::uint32_t name = 0;
    std::uint64_t bits = 0;
};

// Persistence DTO for one generation-local Type entry. TypeEntry::definition
// is an exact one-based range into either enum_values or members, selected by kind.
struct compiled_type_slot {
    bool live = false;
    type_entry record{};
};

// Persistence DTO for one non-static instance member.
struct compiled_member {
    std::uint32_t name = 0;
    std::uint32_t type_ref = 0;
};

// Persistence DTO for one canonical TypeRef table entry.
// The compact 16-byte binary record uses the same logical shape:
//   kind     = canonical_type_kind
//   subtype  = builtin_type or derived_type_kind, depending on kind
//   argument = named type_handle or derived child TypeRef, depending on kind
//   payload  = array extent for derived arrays, otherwise zero
//
// Index in compiled_graph_state::canonical_types is the persisted TypeRef value.
// Import must preserve this table and its indices exactly.
struct compiled_canonical_type {
    std::uint8_t kind = 0;
    std::uint8_t subtype = 0;
    std::uint32_t argument = 0;
    std::uint64_t payload = 0;
};

// Complete persistence projection of canonical Graph state.
// This representation is detached from Graph ownership and may be validated
// transactionally before it is imported into a live Graph.
struct compiled_graph_state {
    abi_configuration abi{};
    std::vector<std::uint32_t> identities;
    std::vector<compiled_entity_slot> entities;
    std::vector<compiled_type_slot> types;
    std::vector<compiled_enum_value> enum_values;
    std::vector<compiled_member> members;
    std::vector<compiled_canonical_type> canonical_types;
};

// Complete compiled checkpoint payload before binary encoding.
// String Registry and Graph are projected together because Graph records refer
// to dense string ids from the accompanying registry projection.
struct compiled_project_state {
    std::vector<std::optional<std::string>> strings;
    compiled_graph_state graph;
};

} // namespace cw::server
