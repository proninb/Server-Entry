#pragma once

#include "../../status.hpp"
#include "../source/source_manager.hpp"
#include "../string/string_registry.hpp"
#include "graph.hpp"
#include "graph_build_transaction.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace cw::server {

class graph_build_transaction_test_access;
class metrics_store;

// Describes whether Graph Manager currently exposes canonical G to Runtime.
// A failed or in-progress build is deliberately non-runnable.
enum class project_state : std::uint8_t {
    valid,
    building,
    error
};

// Owns Source Manager, String Registry, and canonical Graph for one Project and
// coordinates their construction/publication lifetime. Runtime obtains G only
// through runnable_graph(); construction internals remain private.
class graph_manager final {
public:
    graph_manager() = default;

    graph_manager(const graph_manager&) = delete;
    graph_manager& operator=(const graph_manager&) = delete;
    graph_manager(graph_manager&&) = delete;
    graph_manager& operator=(graph_manager&&) = delete;

    [[nodiscard]] status initialize(abi_configuration abi = {}) noexcept;
    [[nodiscard]] graph_build_transaction begin_build() noexcept;

    [[nodiscard]] project_state state() const noexcept {
        return current_state.load(std::memory_order_acquire);
    }

    [[nodiscard]] status runnable_graph(const graph*& output) const noexcept;

    // Storage/introspection view. Runtime must use runnable_graph().
    [[nodiscard]] const graph& compiled_graph() const noexcept;
    [[nodiscard]] const string_registry& strings() const noexcept;

    [[nodiscard]] status save_source_checkpoint(
        const std::filesystem::path& path) const noexcept;

    [[nodiscard]] status save_source_checkpoint(
        const std::filesystem::path& path,
        metrics_store* metrics) const noexcept;

    [[nodiscard]] status load_source_checkpoint(
        const std::filesystem::path& path) noexcept;

    [[nodiscard]] status save_compiled_checkpoint(
        const std::filesystem::path& path,
        metrics_store* metrics = nullptr) const noexcept;

    [[nodiscard]] status load_compiled_checkpoint(
        const std::filesystem::path& path,
        metrics_store* metrics = nullptr) noexcept;

private:
    friend class graph_build_transaction;
    friend class graph_build_transaction_test_access;

    void complete_build(bool success) noexcept;

    source_manager source_manager_state;
    string_registry string_registry_state;
    graph graph_state;

    std::atomic<project_state> current_state{project_state::error};
};

} // namespace cw::server
