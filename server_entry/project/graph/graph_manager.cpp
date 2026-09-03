#include "graph_manager.hpp"
#include "graph_build_transaction.hpp"
#include "compiled_persistence.hpp"
#include "../../metrics/metrics_store.hpp"
#include <chrono>

namespace cw::server
{
status graph_manager::initialize(abi_configuration abi) noexcept
{
    auto result = sources_.initialize();
    if (!result.ok()) return result;
    result = strings_.initialize();
    if (!result.ok()) return result;
    result = graph_.initialize(abi);
    state_.store(project_state::error, std::memory_order_release);
    return result;
}

graph_build_transaction graph_manager::begin_build() noexcept
{
    state_.store(project_state::building, std::memory_order_release);
    return graph_build_transaction{*this};
}

void graph_manager::complete_build(bool success) noexcept
{
    state_.store(success ? project_state::valid : project_state::error,
                 std::memory_order_release);
}

status graph_manager::runnable_graph(const graph*& output) const noexcept
{
    output = nullptr;
    if (state() != project_state::valid)
        return {status_code::invalid_state};
    output = &graph_;
    return {};
}

const graph& graph_manager::compiled_graph() const noexcept
{
    return graph_;
}

const string_registry& graph_manager::strings() const noexcept
{
    return strings_;
}

status graph_manager::save_compiled_checkpoint(const std::filesystem::path& path, metrics_store* metrics) const noexcept
{ return write_compiled_checkpoint(path,strings_,graph_,metrics); }

status graph_manager::load_compiled_checkpoint(const std::filesystem::path& path, metrics_store* metrics) noexcept
{
 state_.store(project_state::building,std::memory_order_release);
 try{string_registry strings;graph graph_state;auto result=strings.initialize();if(result.ok())result=graph_state.initialize();if(result.ok())result=read_compiled_checkpoint(path,strings,graph_state,metrics);if(!result.ok()){state_.store(project_state::error,std::memory_order_release);return result;}const auto begin=std::chrono::steady_clock::now();strings_.swap_compiled(strings);graph_.swap_compiled(graph_state);state_.store(project_state::valid,std::memory_order_release);if(metrics)metrics->record_duration(metric_id::compiled_load_publish_duration,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin));return{};}catch(...){state_.store(project_state::error,std::memory_order_release);return{status_code::initialization_failed};}
}

status graph_manager::save_source_checkpoint(const std::filesystem::path& path) const noexcept
{
    return sources_.save_checkpoint(path);
}

status graph_manager::save_source_checkpoint(
    const std::filesystem::path& path, metrics_store* metrics) const noexcept
{
    return sources_.save_checkpoint(path, metrics);
}

status graph_manager::load_source_checkpoint(const std::filesystem::path& path) noexcept
{
    state_.store(project_state::error, std::memory_order_release);
    return sources_.load_checkpoint(path);
}
} // namespace cw::server
