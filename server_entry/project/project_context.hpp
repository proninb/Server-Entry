#pragma once

#include "../diagnostics/diagnostic_buffer.hpp"
#include "../logging/logger.hpp"
#include "../metrics/metrics_store.hpp"
#include "../operation.hpp"
#include "../status.hpp"
#include "frontend/source_frontend_cache.hpp"
#include "frontend/source_frontend_generation.hpp"
#include "graph/graph_manager.hpp"
#include "runtime/runtime.hpp"
#include "shm/shared_memory.hpp"
#include "source/source_change_tracker.hpp"

#include <atomic>
#include <filesystem>

namespace cw::server {

// Owns one Server Project and its persistent build/runtime state.
// Initial load performs G0. Normal incremental rebuilds consume filesystem
// change notifications and touch only dirty Sources plus their dependents.
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

    [[nodiscard]] status rebuild_sources(
        operation_id operation,
        logger& log,
        metrics_store& metrics) noexcept;

    void shutdown() noexcept;

    [[nodiscard]] const diagnostic_buffer& diagnostics() const noexcept;
    [[nodiscard]] project_state state() const noexcept;
    [[nodiscard]] status runtime_access(const runtime*& output) const noexcept;

    [[nodiscard]] source_frontend_summary
        last_frontend_summary() const noexcept {
        return frontend_summary;
    }

    [[nodiscard]] status load_source_checkpoint(
        const std::filesystem::path& path) noexcept;

    [[nodiscard]] status load_compiled_checkpoint(
        const std::filesystem::path& path,
        metrics_store& metrics) noexcept;

private:
    diagnostic_buffer diagnostic_records;
    graph_manager graphs;
    source_frontend_cache frontend_cache;
    source_change_tracker change_tracker;
    std::filesystem::path active_configuration_path;
    source_frontend_summary frontend_summary;
    runtime runtime_instance;
    shared_memory shared_memory_region;
    std::atomic<bool> runtime_attached{false};
    bool change_tracking_ready = false;
};

} // namespace cw::server
