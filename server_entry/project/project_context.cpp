#include "project_context.hpp"

#include "../diagnostics/diagnostic_descriptor.hpp"
#include "../metrics/scoped_timer.hpp"

namespace cw::server
{

status project_context::initialize(operation_id operation, logger& log,
                                   metrics_store& metrics) noexcept
{
    metrics.increment(metric_id::project_initializations);
    scoped_timer project_timer{metrics, metric_id::project_initialization_duration};
    diagnostics_.clear();
    log.info(log_component::project, operation, "project initialization started");

    auto result = graph_manager_.initialize();
    if (!result.ok())
    {
        diagnostics_.emit({diagnostics::source_initialization_failed.id,
                           diagnostics::source_initialization_failed.default_severity,
                           operation, {}, "while initializing the project"});
        log.error(log_component::source, operation, "source manager initialization failed");
        log.error(log_component::project, operation, "project initialization failed");
        return result;
    }

    metrics.increment(metric_id::shm_initializations);
    {
        scoped_timer shm_timer{metrics, metric_id::shm_initialization_duration};
        result = shared_memory_.initialize();
    }
    if (!result.ok())
    {
        diagnostics_.emit({diagnostics::shm_initialization_failed.id,
                           diagnostics::shm_initialization_failed.default_severity,
                           operation, {}, "while initializing the project"});
        log.error(log_component::shm, operation, "shared memory initialization failed");
        log.error(log_component::project, operation, "project initialization failed");
        return result;
    }

    log.info(log_component::project, operation, "project initialization completed");
    return result;
}

status project_context::load_compiled_checkpoint(const std::filesystem::path& path, metrics_store& metrics) noexcept
{
 auto result=graph_manager_.load_compiled_checkpoint(path,&metrics);if(!result.ok())return result;result=runtime_.attach(graph_manager_.compiled_graph());if(!result.ok())return result;return{};
}

void project_context::shutdown() noexcept
{
}

const diagnostic_buffer& project_context::diagnostics() const noexcept
{
    return diagnostics_;
}

project_state project_context::state() const noexcept
{
    return graph_manager_.state();
}

status project_context::runtime_access(const runtime*& output) const noexcept
{
    output = nullptr;
    const graph* runnable = nullptr;
    const auto result = graph_manager_.runnable_graph(runnable);
    if (!result.ok()) return result;
    output = &runtime_;
    return {};
}

status project_context::load_source_checkpoint(
    const std::filesystem::path& path) noexcept
{
    // Source Manager persistence does not include canonical G. Graph Manager
    // deliberately remains ERROR after a successful Source-only LOAD.
    return graph_manager_.load_source_checkpoint(path);
}

} // namespace cw::server
