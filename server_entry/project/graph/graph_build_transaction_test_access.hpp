#pragma once

#if !defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
#error "graph_build_transaction_test_access requires CW_GRAPH_BUILD_TRANSACTION_TESTING"
#endif

#include "graph_build_transaction.hpp"
#include "graph_manager.hpp"

namespace cw::server {

// Exposes transaction injection points and Graph storage instrumentation only to
// RC/benchmark builds. Production construction APIs remain unchanged.
class graph_build_transaction_test_access final {
public:
    static void set_fail_source_prepare(
        graph_build_transaction& transaction,
        bool enabled = true) noexcept {

        transaction.fail_source_prepare = enabled;
    }

    static void set_fail_string_prepare(
        graph_build_transaction& transaction,
        bool enabled = true) noexcept {

        transaction.fail_string_prepare = enabled;
    }

    static void set_fail_graph_prepare(
        graph_build_transaction& transaction,
        bool enabled = true) noexcept {

        transaction.fail_graph_prepare = enabled;
    }

    static void set_fail_after_graph_prepare(
        graph_build_transaction& transaction,
        bool enabled = true) noexcept {

        transaction.fail_after_graph_prepare = enabled;
    }

    [[nodiscard]] static status remove_named_entity(
        graph_build_transaction& transaction,
        stable_id id) noexcept {

        return transaction.graph_update_state.remove_named_entity_for_testing(id);
    }

    [[nodiscard]] static status prepare(graph_build_transaction& transaction) noexcept {
        return transaction.prepare();
    }

    static void publish_prepared(graph_build_transaction& transaction) noexcept {
        transaction.publish_prepared();
    }

    [[nodiscard]] static graph_build_transaction_state transaction_state(
        const graph_build_transaction& transaction) noexcept {

        return transaction.state;
    }

    [[nodiscard]] static const graph_storage_prepare_telemetry& storage_telemetry(
        const graph_build_transaction& transaction) noexcept {

        return transaction.graph_update_state.storage_prepare_telemetry();
    }

    [[nodiscard]] static graph_storage_snapshot storage_snapshot(
        const graph& value) noexcept {

        return value.storage_snapshot_for_testing();
    }

    [[nodiscard]] static graph_storage_snapshot storage_snapshot(
        const graph_manager& manager) noexcept {

        return manager.graph_state.storage_snapshot_for_testing();
    }

    [[nodiscard]] static const graph& committed_graph(
        const graph_manager& manager) noexcept {

        return manager.graph_state;
    }

    [[nodiscard]] static project_state manager_state(
        const graph_manager& manager) noexcept {

        return manager.current_state.load(std::memory_order_acquire);
    }
};

} // namespace cw::server
