#pragma once

#include "../builder/project_builder.hpp"
#include "../builder/source_build_entry.hpp"
#include "../parser/source_facts.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"

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
// No canonical identity or Graph state is stored here.
struct source_publish_scratch {
    std::vector<enum_value_fact> enum_values;
    std::vector<aggregate_source_fact::member_fact> members;
    std::vector<canonical_type_modifier> modifiers;
    project_builder_scratch builder;
    source_publish_telemetry telemetry;
};

// Captures one Parser batch into Builder-owned Source state. After this returns
// successfully the source_context may be reset immediately.
[[nodiscard]] status capture_source_facts(
    const parser_source_fact_batch& batch,
    source_build_entry& output) noexcept;

// Canonicalizes one captured Source contribution. This function is intentionally
// single-owner: workers prepare source_build_entry objects, while canonical
// String/Entity/TypeRef mutation is performed by the build coordinator.
[[nodiscard]] status publish_source_entry(
    graph_build_transaction& transaction,
    source_build_entry& entry,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept;

[[nodiscard]] status publish_source_entry(
    graph_build_transaction& transaction,
    source_build_entry& entry,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    source_publish_scratch& scratch) noexcept;

// Convenience synchronous boundary for single-threaded callers and tests.
[[nodiscard]] status publish_source_facts(
    graph_build_transaction& transaction,
    const parser_source_fact_batch& batch,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept;

} // namespace cw::server
