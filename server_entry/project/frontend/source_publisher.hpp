#pragma once

#include "../builder/source_build_entry.hpp"
#include "../parser/source_facts.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"

namespace cw::server {

class graph_build_transaction;
class project_builder;

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
    const source_build_entry& entry,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept;

// Convenience synchronous boundary for single-threaded callers and tests.
[[nodiscard]] status publish_source_facts(
    graph_build_transaction& transaction,
    const parser_source_fact_batch& batch,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept;

} // namespace cw::server
