#pragma once

#include "../../status.hpp"
#include "../source/source_manager.hpp"
#include "../string/string_registry.hpp"
#include "graph_build_transaction.hpp"
#include "graph.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace cw::server
{
class metrics_store;
class graph_build_transaction_test_access;

enum class project_state : std::uint8_t
{
    valid,
    building,
    error
};

class graph_manager final
{
public:
    graph_manager() = default;
    graph_manager(const graph_manager&) = delete;
    graph_manager& operator=(const graph_manager&) = delete;
    graph_manager(graph_manager&&) = delete;
    graph_manager& operator=(graph_manager&&) = delete;

    [[nodiscard]] status initialize(abi_configuration abi = {}) noexcept;
    [[nodiscard]] graph_build_transaction begin_build() noexcept;

    [[nodiscard]] project_state state() const noexcept
    { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] status runnable_graph(const graph*& output) const noexcept;
    // Storage/introspection view. Runtime must use runnable_graph().
    [[nodiscard]] const graph& compiled_graph() const noexcept;
    [[nodiscard]] const string_registry& strings() const noexcept;
    [[nodiscard]] status save_source_checkpoint(
        const std::filesystem::path& path) const noexcept;
    [[nodiscard]] status save_source_checkpoint(
        const std::filesystem::path& path, metrics_store* metrics) const noexcept;
    [[nodiscard]] status load_source_checkpoint(
        const std::filesystem::path& path) noexcept;
    [[nodiscard]] status save_compiled_checkpoint(const std::filesystem::path&, metrics_store* = nullptr) const noexcept;
    [[nodiscard]] status load_compiled_checkpoint(const std::filesystem::path&, metrics_store* = nullptr) noexcept;

private:
    friend class graph_build_transaction;
    friend class graph_build_transaction_test_access;
    void complete_build(bool success) noexcept;
    source_manager sources_;
    string_registry strings_;
    graph graph_;
    std::atomic<project_state> state_{project_state::error};
};
} // namespace cw::server
