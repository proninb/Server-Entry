#pragma once

#include "../diagnostics/diagnostic_buffer.hpp"
#include "../logging/logger.hpp"
#include "../metrics/metrics_store.hpp"
#include "../operation.hpp"
#include "../status.hpp"
#include "graph/graph.hpp"
#include "runtime/runtime.hpp"
#include "shm/shared_memory.hpp"
#include "source/source_manager.hpp"

namespace cw::server
{

class project_context
{
public:
    [[nodiscard]] status initialize(operation_id operation, logger& log,
                                    metrics_store& metrics) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] const diagnostic_buffer& diagnostics() const noexcept;

private:
    diagnostic_buffer diagnostics_;
    source_manager source_manager_;
    graph graph_;
    runtime runtime_;
    shared_memory shared_memory_;
};

} // namespace cw::server
