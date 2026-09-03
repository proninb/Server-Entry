#include "graph_build_transaction.hpp"
#include "graph_manager.hpp"

#include <utility>

namespace cw::server
{

graph_build_transaction::graph_build_transaction(graph_manager& owner) noexcept
    : sources_(owner.sources_.begin_update()),
      strings_(owner.strings_.begin_update()),
      graph_(owner.graph_.begin_update()), owner_(&owner)
{
}

graph_build_transaction::~graph_build_transaction()
{
    if (owner_ != nullptr &&
        (state_ == graph_build_transaction_state::active ||
         state_ == graph_build_transaction_state::prepared))
        fail({status_code::invalid_state});
}

graph_build_transaction::graph_build_transaction(graph_build_transaction&& other) noexcept
    : sources_(std::move(other.sources_)), strings_(std::move(other.strings_)),
      graph_(std::move(other.graph_)),
      owner_(std::exchange(other.owner_, nullptr)),
      state_(std::exchange(other.state_, graph_build_transaction_state::failed))
#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    , fail_source_prepare_(other.fail_source_prepare_),
      fail_string_prepare_(other.fail_string_prepare_),
      fail_graph_prepare_(other.fail_graph_prepare_)
#endif
{
}

void graph_build_transaction::fail(status result) noexcept
{
    (void)result;
    sources_.cancel();
    strings_.cancel();
    graph_.cancel();
    state_ = graph_build_transaction_state::failed;
    if (owner_ != nullptr) owner_->complete_build(false);
}

status graph_build_transaction::prepare() noexcept
{
    if (state_ != graph_build_transaction_state::active)
        return {status_code::invalid_state};

#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    if (fail_source_prepare_)
    {
        const status result{status_code::initialization_failed};
        fail(result);
        return result;
    }
#endif
    auto result = sources_.prepare_publish();
    if (!result.ok())
    {
        fail(result);
        return result;
    }

#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    if (fail_string_prepare_)
    {
        const status forced{status_code::initialization_failed};
        fail(forced);
        return forced;
    }
#endif
    result = strings_.prepare_publish();
    if (!result.ok())
    {
        fail(result);
        return result;
    }
#ifdef CW_GRAPH_BUILD_TRANSACTION_TESTING
    if (fail_graph_prepare_)
    {
        const status forced{status_code::initialization_failed};
        fail(forced);
        return forced;
    }
#endif
    result = graph_.prepare_publish(sources_, strings_);
    if (!result.ok())
    {
        fail(result);
        return result;
    }
    state_ = graph_build_transaction_state::prepared;
    return {};
}

void graph_build_transaction::publish_prepared() noexcept
{
    // Publication order is Source Manager, String Registry, then Graph.
    // Every operation below this barrier is a preflighted no-fail transfer.
    sources_.publish_prepared();
    strings_.publish_prepared();
    graph_.publish_prepared();
    state_ = graph_build_transaction_state::committed;
    if (owner_ != nullptr) owner_->complete_build(true);
}

status graph_build_transaction::commit() noexcept
{
    const auto result = prepare();
    if (!result.ok()) return result;
    publish_prepared();
    return {};
}

status graph_build_transaction::checkpoint_sources(
    const std::filesystem::path& path, metrics_store* metrics) noexcept
{
    if (state_ != graph_build_transaction_state::committed || owner_ == nullptr)
        return {status_code::invalid_state};
    return owner_->save_source_checkpoint(path, metrics);
}

status graph_build_transaction::checkpoint_compiled(
    const std::filesystem::path& path, metrics_store* metrics) noexcept
{
    if (state_ != graph_build_transaction_state::committed || owner_ == nullptr)
        return {status_code::invalid_state};
    return owner_->save_compiled_checkpoint(path, metrics);
}

} // namespace cw::server
