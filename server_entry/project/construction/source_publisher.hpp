#pragma once

#include "../parser/source_facts.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"

namespace cw::server
{
class graph_build_transaction;
class project_builder;

[[nodiscard]] status publish_source_facts(
    graph_build_transaction& transaction,
    const parser_source_fact_batch& source,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept;
}
