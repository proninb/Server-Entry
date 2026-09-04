#pragma once

#include "../../status.hpp"
#include "../source/source_manager.hpp"
#include "../string/string_registry.hpp"
#include "graph.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace cw::server {

class diagnostic_buffer;
class graph_build_transaction;
class graph_build_transaction_test_access;
class graph_manager;
class metrics_store;
class operation_id;
class project_builder;
class source_frontend_generation;
struct parser_source_fact_batch;

[[nodiscard]] status publish_source_facts(
    graph_build_transaction& transaction,
    const parser_source_fact_batch& facts,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept;

// Describes the lifecycle of one coordinated Project build transaction.
// A prepared transaction has completed every fallible operation required by
// Source Manager, String Registry, and canonical Graph publication.
enum class graph_build_transaction_state : std::uint8_t {
    active,
    prepared,
    committed,
    failed
};

// Coordinates one fail-closed logical publication across Source Manager,
// String Registry, and canonical Graph. All validation/allocation is completed
// by prepare(); publish_prepared() is the fixed no-fail publication barrier.
class graph_build_transaction final {
public:
    ~graph_build_transaction();

    graph_build_transaction(const graph_build_transaction&) = delete;
    graph_build_transaction& operator=(const graph_build_transaction&) = delete;

    graph_build_transaction(graph_build_transaction&& other) noexcept;
    graph_build_transaction& operator=(graph_build_transaction&&) = delete;

    [[nodiscard]] source_manager_update& sources() noexcept {
        return source_update;
    }

    [[nodiscard]] string_registry_update& strings() noexcept {
        return string_update;
    }

    [[nodiscard]] graph_update& graph_state() noexcept {
        return graph_update_state;
    }

    [[nodiscard]] status commit() noexcept;

private:
    friend class graph_manager;
    friend class graph_build_transaction_test_access;
    friend class project_builder;
    friend class source_frontend_generation;

    friend status publish_source_facts(
        graph_build_transaction& transaction,
        const parser_source_fact_batch& facts,
        const project_builder& builder,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    explicit graph_build_transaction(graph_manager& owner) noexcept;

    [[nodiscard]] status prepare() noexcept;
    void publish_prepared() noexcept;
    void fail(status result) noexcept;

    [[nodiscard]] status checkpoint_sources(
        const std::filesystem::path& path,
        metrics_store* metrics) noexcept;

    [[nodiscard]] status checkpoint_compiled(
        const std::filesystem::path& path,
        metrics_store* metrics) noexcept;

    source_manager_update source_update;
    string_registry_update string_update;
    graph_update graph_update_state;

    graph_manager* owner = nullptr;
    graph_build_transaction_state state = graph_build_transaction_state::active;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    bool fail_source_prepare = false;
    bool fail_string_prepare = false;
    bool fail_graph_prepare = false;

    // Injection point after Graph has completed all reserve/validation work but
    // before any candidate layer crosses the publication barrier.
    bool fail_after_graph_prepare = false;
#endif
};

} // namespace cw::server
