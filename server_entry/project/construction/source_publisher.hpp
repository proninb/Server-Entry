#pragma once

#include "../parser/source_facts.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"

namespace cw::server {

    class graph_build_transaction;
    class project_builder;

    // Canonicalizes one transient Parser fact batch into the active Graph build
    // transaction. The function resolves source_context name references only into
    // String Registry ids and delegates Entity identity, TypeRef creation, ABI
    // semantics, and Graph mutation to Builder/Graph.
    //
    // The batch is consumed synchronously; no source_context-owned memory is retained.
    [[nodiscard]] status publish_source_facts(
        graph_build_transaction& transaction,
        const parser_source_fact_batch& batch,
        const project_builder& builder,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

} // namespace cw::server
