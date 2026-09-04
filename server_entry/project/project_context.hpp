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

namespace cw::server {

// Owns the lifetime and major runtime-facing components of one Server project.
// project_context coordinates Graph Manager, Runtime, Shared Memory, and project
// diagnostics while delegating construction, graph state, runtime attachment,
// and SHM behavior to their owning subsystems.
class project_context {
public:
    [[nodiscard]] status initialize(
        operation_id operation,
        logger& log,
        metrics_store& metrics) noexcept;

    void shutdown() noexcept;

    [[nodiscard]] const diagnostic_buffer& diagnostics() const noexcept;
    [[nodiscard]] project_state state() const noexcept;
    [[nodiscard]] status runtime_access(const runtime*& output) const noexcept;

    [[nodiscard]] status load_source_checkpoint(
        const std::filesystem::path& path) noexcept;

    [[nodiscard]] status load_compiled_checkpoint(
        const std::filesystem::path& path,
        metrics_store& metrics) noexcept;

private:
    diagnostic_buffer diagnostic_records;
    graph_manager graphs;
    runtime runtime_instance;
    shared_memory shared_memory_region;
};

} // namespace cw::server
