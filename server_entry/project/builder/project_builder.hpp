#pragma once

#include "canonical_source_facts.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../graph/graph.hpp"

#include <span>

namespace cw::server {

class graph_build_transaction;

// Converts canonical source facts into canonical Graph state.
// Parser/source_publisher must already have resolved source-language names before
// they reach this layer. Builder owns canonical Entity/TypeRef construction,
// ABI-dependent enum materialization, member type composition, and Graph invariants;
// it must not perform source-language namespace or visibility lookup.
class project_builder final {
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
