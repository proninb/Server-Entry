#pragma once

#include "../../status.hpp"
#include "../builder/source_contribution_cache.hpp"
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

// Describes whether Graph Manager currently exposes a canonical Graph to Runtime.
// error is the fail-closed/non-runnable state and is also used after initialization,
// source-only checkpoint load, or any failed build/compiled-checkpoint load.
enum class project_state : std::uint8_t {
    valid,
    building,
    error
};

// Owns the coordinated Source Manager, String Registry, and canonical Graph state
// for one Project. Graph Manager creates build transactions, publishes the Runtime-
// visible validity state, and performs transactional compiled-checkpoint replacement.
// It remains platform-neutral; filesystem mechanics are delegated to persistence.
class graph_manager final {
public:
    graph_manager() = default;

    graph_manager(const graph_manager&) = delete;
    graph_manager& operator=(const graph_manager&) = delete;

    graph_manager(graph_manager&&) = delete;
    graph_manager& operator=(graph_manager&&) = delete;

    // Initializes owned storage but does not make Graph runnable. Runtime becomes
    // eligible only after a successful build or compiled-checkpoint load.
    [[nodiscard]] status initialize(
        abi_configuration abi = {}) noexcept;

    // Starts one coordinated candidate build and immediately makes the currently
    // exposed Graph non-runnable until that transaction completes.
    // The caller must preserve the Graph Manager single-writer build contract.
    [[nodiscard]] graph_build_transaction begin_build(
        graph_build_mode mode = graph_build_mode::rebuild) noexcept;

    [[nodiscard]] project_state state() const noexcept {
        return current_state.load(
            std::memory_order_acquire);
    }

    [[nodiscard]] status runnable_graph(
        const graph*& output) const noexcept;

    // Storage/introspection view. Runtime must use runnable_graph().
    [[nodiscard]] const graph& compiled_graph() const noexcept;

    [[nodiscard]] const string_registry& strings() const noexcept;

    [[nodiscard]] status save_source_checkpoint(
        const std::filesystem::path& path) const noexcept;

    [[nodiscard]] status save_source_checkpoint(
        const std::filesystem::path& path,
        metrics_store* metrics) const noexcept;

    // Restores Source Manager state only. No canonical G is published, so the
    // project deliberately remains non-runnable afterward.
    [[nodiscard]] status load_source_checkpoint(
        const std::filesystem::path& path) noexcept;

    [[nodiscard]] status save_compiled_checkpoint(
        const std::filesystem::path& path,
        metrics_store* metrics = nullptr) const noexcept;

    // Loads String Registry + canonical Graph into detached candidates and swaps
    // them into live ownership only after the complete artifact validates/imports.
    [[nodiscard]] status load_compiled_checkpoint(
        const std::filesystem::path& path,
        metrics_store* metrics = nullptr) noexcept;

private:
    friend class graph_build_transaction;
    friend class graph_build_transaction_test_access;

    void complete_build(bool success) noexcept;

    source_manager source_manager_state;
    string_registry string_registry_state;
    source_contribution_cache source_contribution_cache_state;
    graph graph_state;

    std::atomic<project_state> current_state{
        project_state::error
    };
};

} // namespace cw::server
