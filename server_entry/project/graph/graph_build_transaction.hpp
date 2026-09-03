#pragma once

#include "../../status.hpp"
#include "../source/source_manager.hpp"
#include "../string/string_registry.hpp"
#include "graph.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace cw::server
{

class graph_manager;
class graph_build_transaction;
class graph_build_transaction_test_access;
class project_builder;
class source_frontend_generation;
struct parser_source_fact_batch;
class diagnostic_buffer;
class operation_id;
class metrics_store;
[[nodiscard]] status publish_source_facts(
    graph_build_transaction&, const parser_source_fact_batch&,
    const project_builder&, operation_id, diagnostic_buffer&) noexcept;

enum class graph_build_transaction_state : std::uint8_t
{
    active,
    prepared,
    committed,
    failed
};

class graph_build_transaction final
{
public:
    ~graph_build_transaction();
    graph_build_transaction(const graph_build_transaction&) = delete;
    graph_build_transaction& operator=(const graph_build_transaction&) = delete;
    graph_build_transaction(graph_build_transaction&& other) noexcept;
    graph_build_transaction& operator=(graph_build_transaction&&) = delete;

    [[nodiscard]] source_manager_update& sources() noexcept { return sources_; }
    [[nodiscard]] string_registry_update& strings() noexcept { return strings_; }
    [[nodiscard]] graph_update& graph_state() noexcept { return graph_; }
    [[nodiscard]] status commit() noexcept;

private:
    friend class graph_manager;
    friend class graph_build_transaction_test_access;
    friend class project_builder;
    friend class source_frontend_generation;
    friend status publish_source_facts(
        graph_build_transaction&, const parser_source_fact_batch&,
        const project_builder&, operation_id, diagnostic_buffer&) noexcept;
    explicit graph_build_transaction(graph_manager& owner) noexcept;

    [[nodiscard]] status prepare() noexcept;
    void publish_prepared() noexcept;
    void fail(status result) noexcept;
    [[nodiscard]] status checkpoint_sources(
        const std::filesystem::path& path, metrics_store* metrics) noexcept;
    [[nodiscard]] status checkpoint_compiled(
        const std::filesystem::path& path, metrics_store* metrics) noexcept;

    source_manager_update sources_;
    string_registry_update strings_;
    graph_update graph_;
    graph_manager* owner_ = nullptr;
    graph_build_transaction_state state_ = graph_build_transaction_state::active;

#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    bool fail_source_prepare_ = false;
    bool fail_string_prepare_ = false;
    bool fail_graph_prepare_ = false;
#endif
};

} // namespace cw::server
