#pragma once

#include "../diagnostics/diagnostic_buffer.hpp"
#include "../logging/logger.hpp"
#include "../metrics/metrics_store.hpp"
#include "../operation.hpp"
#include "../status.hpp"
#include "graph/graph_manager.hpp"
#include "runtime/runtime.hpp"
#include "shm/shared_memory.hpp"

#include <atomic>
#include <filesystem>

namespace cw::server {

// Owns the lifetime and major runtime-facing components of one Server project.
// project_context orchestrates Project configuration/composition, Source frontend,
// canonical Graph publication, Runtime attachment, and Shared Memory lifetime.
class project_context {
public:
    [[nodiscard]] status initialize(
        operation_id operation,
        logger& log,
        metrics_store& metrics) noexcept;

    [[nodiscard]] status load_project(
        const std::filesystem::path& configuration_path,
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
    std::atomic<bool> runtime_attached{false};
};

} // namespace cw::server
