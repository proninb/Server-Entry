#pragma once

#include "../builder/project_builder.hpp"
#include "../builder/source_build_entry.hpp"
#include "../string/string_registry.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../../string_id.hpp"

#include <cstdint>
#include <vector>

namespace cw::server {

class graph_build_transaction;

// Detailed publication telemetry is local to one frontend generation. Basic
// production publication selects an untimed implementation at the Source
// boundary and performs no per-fact clock reads.
struct source_publish_telemetry {
    bool enabled = false;

    std::uint64_t source_replace_ns = 0;
    std::uint64_t enum_name_ns = 0;
    std::uint64_t enum_values_ns = 0;
    std::uint64_t enum_sample_name_resolve_ns = 0;
    std::uint64_t enum_sample_name_intern_ns = 0;
    std::uint64_t enum_sample_values_resolve_ns = 0;
    std::uint64_t enum_sample_values_intern_ns = 0;
    std::uint64_t enum_builder_ns = 0;
    std::uint64_t aggregate_builder_ns = 0;

    std::uint64_t source_count = 0;
    std::uint64_t enum_builder_count = 0;
    std::uint64_t aggregate_builder_count = 0;
};

// Reusable COLD buffers for one deterministic single-owner publication pass.
// name_bindings is the transient lexical-name -> canonical string_id boundary;
// no canonical identity is written back into Parser-owned Source facts.
struct source_publish_scratch {
    std::vector<string_id> name_bindings;
    std::vector<std::uint8_t> name_binding_seen;
    std::vector<prehashed_string_binding> string_bindings;
    std::vector<std::uint32_t> string_binding_slots;
    std::vector<string_id> string_binding_results;
    std::vector<enum_value_fact> enum_values;
    std::vector<aggregate_source_fact::member_fact> members;
    std::vector<canonical_type_modifier> modifiers;
    project_builder_scratch builder;
    source_publish_telemetry telemetry;
};

// Canonicalizes one immutable Source semantic product. Workers construct facts;
// the coordinator alone binds project strings and mutates canonical Graph state.
[[nodiscard]] status publish_source_entry(
    graph_build_transaction& transaction,
    const source_build_entry& entry,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept;

[[nodiscard]] status publish_source_entry(
    graph_build_transaction& transaction,
    const source_build_entry& entry,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    source_publish_scratch& scratch) noexcept;

} // namespace cw::server
