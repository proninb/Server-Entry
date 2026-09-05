#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace cw::server {

class source_manager;

// One fail-closed batch produced by Source Change Tracker.
// dirty_sources contains only known Project Sources. rescan_required means
// event continuity is not trustworthy and the caller must reconcile all Sources.
struct source_change_batch {
    std::vector<source_id> dirty_sources;
    bool rescan_required = false;
    bool project_configuration_changed = false;
};

// Watches committed Source paths using Windows directory change notifications.
// Notifications are acceleration only: overflow, watcher failure, or restart
// requests a full Source reconciliation instead of risking a missed change.
class source_change_tracker final {
public:
    source_change_tracker();
    ~source_change_tracker();

    source_change_tracker(const source_change_tracker&) = delete;
    source_change_tracker& operator=(const source_change_tracker&) = delete;

    [[nodiscard]] status initialize(
        const source_manager& sources,
        const std::filesystem::path& project_configuration) noexcept;

    // Adds newly discovered Source paths after a successful generation.
    // Existing watches remain active, avoiding a notification gap.
    [[nodiscard]] status synchronize(
        const source_manager& sources) noexcept;

    // Drains the current batch. A short grace period lets a just-completed file
    // write reach the already-armed ReadDirectoryChangesW request.
    [[nodiscard]] status drain(
        source_change_batch& output,
        std::chrono::milliseconds grace =
            std::chrono::milliseconds{2}) noexcept;

    void require_rescan() noexcept;
    void stop() noexcept;

    [[nodiscard]] bool active() const noexcept;

private:
    class implementation;
    std::unique_ptr<implementation> state;
};

} // namespace cw::server
