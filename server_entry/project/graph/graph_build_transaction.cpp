#include "graph_build_transaction.hpp"

#include "graph_manager.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace cw::server {
namespace {

std::uint64_t transaction_elapsed_ns(
    std::chrono::steady_clock::time_point begin) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count());
}

} // namespace

graph_build_transaction::graph_build_transaction(
    graph_manager& graph_manager,
    graph_build_mode mode) noexcept
    : source_update(graph_manager.source_manager_state.begin_update()),
      string_update(graph_manager.string_registry_state.begin_update()),
      contribution_update(
          graph_manager.source_contribution_cache_state.begin_update(
              mode == graph_build_mode::rebuild)),
      graph_update_state(
          graph_manager.graph_state.begin_update(
              mode,
              contribution_update)),
      owner(&graph_manager) {}

graph_build_transaction::~graph_build_transaction() {
    if (owner != nullptr &&
        (state == graph_build_transaction_state::active ||
         state == graph_build_transaction_state::prepared)) {
        fail({status_code::invalid_state});
    }
}

graph_build_transaction::graph_build_transaction(
    graph_build_transaction&& other) noexcept
    : source_update(std::move(other.source_update)),
      string_update(std::move(other.string_update)),
      contribution_update(std::move(other.contribution_update)),
      graph_update_state(std::move(other.graph_update_state)),
      owner(std::exchange(other.owner, nullptr)),
      timings(other.timings),
      state(std::exchange(
          other.state,
          graph_build_transaction_state::failed))
#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
      ,
      fail_source_prepare(other.fail_source_prepare),
      fail_string_prepare(other.fail_string_prepare),
      fail_graph_prepare(other.fail_graph_prepare),
      fail_after_graph_prepare(other.fail_after_graph_prepare)
#endif
{
    // graph_update's dependency is another member of this transaction. Moving
    // its pointer alone would retain the moved-from contribution update.
    graph_update_state.contributions = &contribution_update;
}

void graph_build_transaction::fail(status result) noexcept {

    // The originating operation returns the detailed status to its caller.
    // Transaction failure only guarantees that every candidate is cancelled and
    // that Graph Manager leaves its build-in-progress state consistently.
    (void)result;

    source_update.cancel();
    string_update.cancel();
    contribution_update.cancel();
    graph_update_state.cancel();

    state = graph_build_transaction_state::failed;

    if (owner != nullptr) {
        owner->complete_build(false);
    }
}

status graph_build_transaction::prepare() noexcept {

    if (state != graph_build_transaction_state::active) {
        return {status_code::invalid_state};
    }

#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    if (fail_source_prepare) {
        const status result{
            status_code::initialization_failed
        };

        fail(result);
        return result;
    }
#endif

    auto phase_begin =
        std::chrono::steady_clock::now();

    auto result =
        source_update.prepare_publish();

    timings.source_prepare_ns =
        transaction_elapsed_ns(phase_begin);

    if (!result.ok()) {
        fail(result);
        return result;
    }

#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    if (fail_string_prepare) {
        const status forced{
            status_code::initialization_failed
        };

        fail(forced);
        return forced;
    }
#endif

    phase_begin =
        std::chrono::steady_clock::now();

    result =
        string_update.prepare_publish();

    timings.string_prepare_ns =
        transaction_elapsed_ns(phase_begin);

    if (!result.ok()) {
        fail(result);
        return result;
    }

#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    if (fail_graph_prepare) {
        const status forced{
            status_code::initialization_failed
        };

        fail(forced);
        return forced;
    }
#endif

    phase_begin =
        std::chrono::steady_clock::now();

    result =
        graph_update_state.prepare_publish(
            source_update,
            string_update);

    timings.graph_prepare_ns =
        transaction_elapsed_ns(phase_begin);

    const auto& graph_timing =
        graph_update_state.prepare_phase_telemetry();

    timings.graph_pending_member_resolution_ns =
        graph_timing.pending_member_resolution_ns;
    timings.graph_live_typeref_validation_ns =
        graph_timing.live_typeref_validation_ns;
    timings.graph_canonical_typeref_rebuild_ns =
        graph_timing.canonical_typeref_rebuild_ns;
    timings.graph_string_validation_ns =
        graph_timing.string_validation_ns;
    timings.graph_definition_scan_ns =
        graph_timing.definition_scan_ns;
    timings.graph_definition_materialization_ns =
        graph_timing.definition_materialization_ns;
    timings.graph_rebuild_storage_ns =
        graph_timing.rebuild_storage_ns;
    timings.graph_dependency_index_ns =
        graph_timing.dependency_index_ns;
    timings.graph_final_prepare_ns =
        graph_timing.final_prepare_ns;

    if (!result.ok()) {
        fail(result);
        return result;
    }

#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    if (fail_after_graph_prepare) {
        const status forced{
            status_code::initialization_failed
        };

        fail(forced);
        return forced;
    }
#endif

    std::vector<std::uint8_t> retained_strings;

    phase_begin =
        std::chrono::steady_clock::now();

    result = graph_update_state.collect_rebuild_string_retention(
        string_update.candidate_size_for_validation(),
        retained_strings);

    timings.string_retention_ns =
        transaction_elapsed_ns(phase_begin);

    if (!result.ok()) {
        fail(result);
        return result;
    }

    if (!retained_strings.empty()) {
        phase_begin =
            std::chrono::steady_clock::now();

        result = string_update.prepare_rebuild_compaction(
            retained_strings);

        timings.string_compaction_ns =
            transaction_elapsed_ns(phase_begin);

        if (!result.ok()) {
            fail(result);
            return result;
        }
    }

    phase_begin =
        std::chrono::steady_clock::now();

    result = contribution_update.prepare_publish();

    timings.contribution_prepare_ns =
        transaction_elapsed_ns(phase_begin);

    if (!result.ok()) {
        fail(result);
        return result;
    }

    state =
        graph_build_transaction_state::prepared;

    return {};
}

void graph_build_transaction::publish_prepared() noexcept {

    // Publication order is part of the transaction contract. Graph validation
    // has already observed the prepared Source Manager and String Registry
    // candidates, so those dependencies become committed before Graph itself.
    auto phase_begin =
        std::chrono::steady_clock::now();

    source_update.publish_prepared();

    timings.source_publish_ns =
        transaction_elapsed_ns(phase_begin);

    phase_begin =
        std::chrono::steady_clock::now();

    string_update.publish_prepared();

    timings.string_publish_ns =
        transaction_elapsed_ns(phase_begin);

    phase_begin =
        std::chrono::steady_clock::now();

    contribution_update.publish_prepared();

    timings.contribution_publish_ns =
        transaction_elapsed_ns(phase_begin);

    phase_begin =
        std::chrono::steady_clock::now();

    graph_update_state.publish_prepared();

    timings.graph_publish_ns =
        transaction_elapsed_ns(phase_begin);

    state =
        graph_build_transaction_state::committed;

    if (owner != nullptr) {
        owner->complete_build(true);
    }
}

status graph_build_transaction::commit() noexcept {
    const auto result = prepare();

    if (!result.ok()) {
        return result;
    }

    publish_prepared();
    return {};
}

status graph_build_transaction::checkpoint_sources(
    const std::filesystem::path& path,
    metrics_store* metrics) noexcept {


    if (state != graph_build_transaction_state::committed ||
        owner == nullptr) {
        return {status_code::invalid_state};
    }

    return owner->save_source_checkpoint(
        path,
        metrics);
}

status graph_build_transaction::checkpoint_compiled(
    const std::filesystem::path& path,
    metrics_store* metrics) noexcept {


    if (state != graph_build_transaction_state::committed ||
        owner == nullptr) {
        return {status_code::invalid_state};
    }

    return owner->save_compiled_checkpoint(
        path,
        metrics);
}

} // namespace cw::server
