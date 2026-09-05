#pragma once

#include "graph_manager.hpp"

#if !defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
#error "graph_build_transaction_test_access.hpp is test-only"
#endif

namespace cw::server {

// Test/benchmark-only access to transaction preparation, publication, telemetry,
// and storage snapshots. This surface is excluded from production builds.
class graph_build_transaction_test_access final {
public:
    [[nodiscard]] static status prepare(
        graph_build_transaction& transaction) noexcept {

        return transaction.prepare();
    }

    static void publish(
        graph_build_transaction& transaction) noexcept {

        transaction.publish_prepared();
    }

    [[nodiscard]] static graph_build_transaction_state state(
        const graph_build_transaction& transaction) noexcept {

        return transaction.state;
    }

    static void fail_after_graph_prepare(
        graph_build_transaction& transaction) noexcept {

        transaction.fail_after_graph_prepare = true;
    }

    [[nodiscard]] static const graph_storage_prepare_telemetry& graph_telemetry(
        const graph_build_transaction& transaction) noexcept {

        return transaction.graph_update_state.storage_telemetry;
    }

    [[nodiscard]] static graph_storage_snapshot graph_storage(
        const graph_manager& manager) noexcept {

        return manager.graph_state.storage_snapshot_for_testing();
    }

    [[nodiscard]] static source_contribution_storage_snapshot
    contribution_storage(
        const graph_manager& manager) noexcept {

        return manager.source_contribution_cache_state
            .storage_snapshot_for_testing();
    }

    [[nodiscard]] static std::uint64_t graph_generation(
        const graph_manager& manager) noexcept {

        return manager.graph_state.generation;
    }

    [[nodiscard]] static std::size_t contribution_count(
        const graph_manager& manager,
        source_id source) noexcept {

        return manager.source_contribution_cache_state
            .contribution_count(source);
    }
};

} // namespace cw::server
