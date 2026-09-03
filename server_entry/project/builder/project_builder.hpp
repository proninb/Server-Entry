#pragma once

#include "canonical_source_facts.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../graph/graph.hpp"

namespace cw::server
{

class graph_build_transaction;

class project_builder final
{
public:
    // Canonical-fact seam retained for focused Builder tests and non-Parser
    // producers. Production Parser ingestion goes through source_publisher.
    [[nodiscard]] status build(
        graph_build_transaction& transaction,
        std::span<const source_fact_batch> sources,
        operation_id operation,
        diagnostic_buffer& diagnostics) const noexcept;
    [[nodiscard]] status build_enum(
        graph_update::source_replacement& replacement,
        const enum_source_fact& fact) const noexcept;
    [[nodiscard]] status build_aggregate(
        graph_update::source_replacement& replacement,
        const aggregate_source_fact& fact) const noexcept;

};

} // namespace cw::server
