#include "graph_build_transaction.hpp"

#include "graph_manager.hpp"

#include <utility>

namespace cw::server {

graph_build_transaction::graph_build_transaction(graph_manager& manager) noexcept
    : source_update(manager.source_manager_state.begin_update()),
      string_update(manager.string_registry_state.begin_update()),
      graph_update_state(manager.graph_state.begin_update()),
      owner(&manager) {
}

graph_build_transaction::~graph_build_transaction() {
    if (owner != nullptr &&
        (state == graph_build_transaction_state::active ||
         state == graph_build_transaction_state::prepared)) {
        fail({status_code::invalid_state});
    }
}

graph_build_transaction::graph_build_transaction(graph_build_transaction&& other) noexcept
    : source_update(std::move(other.source_update)),
      string_update(std::move(other.string_update)),
      graph_update_state(std::move(other.graph_update_state)),
      owner(std::exchange(other.owner, nullptr)),
      state(std::exchange(other.state, graph_build_transaction_state::failed))
#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
      , fail_source_prepare(other.fail_source_prepare),
      fail_string_prepare(other.fail_string_prepare),
      fail_graph_prepare(other.fail_graph_prepare),
      fail_after_graph_prepare(other.fail_after_graph_prepare)
#endif
{
}

void graph_build_transaction::fail(status result) noexcept {
    (void)result;

    source_update.cancel();
    string_update.cancel();
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

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    if (fail_source_prepare) {
        const status result{status_code::initialization_failed};
        fail(result);
        return result;
    }
#endif

    auto result = source_update.prepare_publish();

    if (!result.ok()) {
        fail(result);
        return result;
    }

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    if (fail_string_prepare) {
        const status forced{status_code::initialization_failed};
        fail(forced);
        return forced;
    }
#endif

    result = string_update.prepare_publish();

    if (!result.ok()) {
        fail(result);
        return result;
    }

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    if (fail_graph_prepare) {
        const status forced{status_code::initialization_failed};
        fail(forced);
        return forced;
    }
#endif

    result = graph_update_state.prepare_publish(source_update, string_update);

    if (!result.ok()) {
        fail(result);
        return result;
    }

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    if (fail_after_graph_prepare) {
        const status forced{status_code::initialization_failed};
        fail(forced);
        return forced;
    }
#endif

    state = graph_build_transaction_state::prepared;
    return {};
}

void graph_build_transaction::publish_prepared() noexcept {
    // Graph validation has already observed the prepared Source Manager and
    // String Registry candidates. No fallible operation is allowed below here.
    source_update.publish_prepared();
    string_update.publish_prepared();
    graph_update_state.publish_prepared();

    state = graph_build_transaction_state::committed;

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

    if (state != graph_build_transaction_state::committed || owner == nullptr) {
        return {status_code::invalid_state};
    }

    return owner->save_source_checkpoint(path, metrics);
}

status graph_build_transaction::checkpoint_compiled(
    const std::filesystem::path& path,
    metrics_store* metrics) noexcept {

    if (state != graph_build_transaction_state::committed || owner == nullptr) {
        return {status_code::invalid_state};
    }

    return owner->save_compiled_checkpoint(path, metrics);
}

} // namespace cw::server
