#pragma once

#include "../diagnostics/diagnostic_buffer.hpp"
#include "../logging/logger.hpp"
#include "../metrics/metrics_store.hpp"
#include "../operation.hpp"
#include "../status.hpp"
#include "graph/graph_manager.hpp"
#include "runtime/runtime.hpp"
#include "shm/shared_memory.hpp"

#include <filesystem>

namespace cw::server
{

class project_context
{
public:
    [[nodiscard]] status initialize(operation_id operation, logger& log,
                                    metrics_store& metrics) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] const diagnostic_buffer& diagnostics() const noexcept;
    [[nodiscard]] project_state state() const noexcept;
    [[nodiscard]] status runtime_access(const runtime*& output) const noexcept;
    [[nodiscard]] status load_source_checkpoint(
        const std::filesystem::path& path) noexcept;
    [[nodiscard]] status load_compiled_checkpoint(const std::filesystem::path&, metrics_store&) noexcept;

private:
    diagnostic_buffer diagnostics_;
    graph_manager graph_manager_;
    runtime runtime_;
    shared_memory shared_memory_;
};

} // namespace cw::server
