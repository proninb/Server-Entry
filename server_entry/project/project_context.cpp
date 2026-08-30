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

    auto result = source_manager_.initialize();
    if (!result.ok())
    {
        diagnostics_.emit({diagnostics::source_initialization_failed.id,
                           diagnostics::source_initialization_failed.default_severity,
                           operation, {}, "while initializing the project"});
        log.error(log_component::source, operation, "source manager initialization failed");
        log.error(log_component::project, operation, "project initialization failed");
        return result;
    }

    metrics.increment(metric_id::runtime_attach_count);
    {
        scoped_timer runtime_timer{metrics, metric_id::runtime_attach_duration};
        result = runtime_.attach(graph_);
    }
    if (!result.ok())
    {
        diagnostics_.emit({diagnostics::runtime_attach_failed.id,
                           diagnostics::runtime_attach_failed.default_severity,
                           operation, {}, "while initializing the project"});
        log.error(log_component::runtime, operation, "runtime attach failed");
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

void project_context::shutdown() noexcept
{
}

const diagnostic_buffer& project_context::diagnostics() const noexcept
{
    return diagnostics_;
}

} // namespace cw::server
