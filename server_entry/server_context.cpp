#include "server_context.hpp"

#include "metrics/scoped_timer.hpp"

#include <utility>

namespace cw::server
{

server_context::server_context() : server_context(server_configuration{})
{
}

server_context::server_context(server_configuration configuration)
    : config_(std::move(configuration))
{
    metrics_.set_mode(config_.telemetry.metrics ? metrics_mode::basic
                                                : metrics_mode::off);
    logger_.set_minimum_level(config_.logging.minimum_level);
    if (config_.logging.console)
    {
        logger_.add_sink(console_sink_);
    }
}

status server_context::initialize() noexcept
{
    metrics_.increment(metric_id::server_initializations);
    scoped_timer initialization_timer{metrics_, metric_id::server_initialization_duration};
    const auto operation = next_operation_id();
    logger_.info(log_component::server, operation, "server initialization started");

    const auto result = project_.initialize(operation, logger_, metrics_);
    if (!result.ok())
    {
        logger_.error(log_component::server, operation, "server initialization failed");
        return result;
    }

    logger_.info(log_component::server, operation, "server initialization completed");
    metrics_.set(metric_id::server_active_projects, 1);
    return result;
}

void server_context::shutdown() noexcept
{
    const auto operation = next_operation_id();
    logger_.info(log_component::server, operation, "server shutdown started");
    project_.shutdown();
    metrics_.set(metric_id::server_active_projects, 0);
    logger_.info(log_component::server, operation, "server shutdown completed");
}

operation_id server_context::next_operation_id() noexcept
{
    return operation_id{next_operation_value_++};
}

} // namespace cw::server
