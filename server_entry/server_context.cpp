#include "server_context.hpp"

#include "metrics/scoped_timer.hpp"

#include <utility>

namespace cw::server {

server_context::server_context() : server_context(server_configuration{}) {}

server_context::server_context(server_configuration configuration)
    : config(std::move(configuration)) {
    metrics.set_mode(config.telemetry.metrics ? metrics_mode::basic
                                              : metrics_mode::off);
    logger.set_minimum_level(config.logging.minimum_level);
    if (config.logging.console) {
        logger.add_sink(console_sink);
    }
}

status server_context::initialize() noexcept {
    metrics.increment(metric_id::server_initializations);
    scoped_timer initialization_timer{metrics, metric_id::server_initialization_duration};

    const auto operation = next_operation_id();
    logger.info(log_component::server, operation, "server initialization started");

    const auto result = project.initialize(operation, logger, metrics);
    if (!result.ok()) {
        logger.error(log_component::server, operation, "server initialization failed");
        return result;
    }

    logger.info(log_component::server, operation, "server initialization completed");
    metrics.set(metric_id::server_active_projects, 1);

    return result;
}

void server_context::shutdown() noexcept {
    const auto operation = next_operation_id();
    logger.info(log_component::server, operation, "server shutdown started");

    project.shutdown();
    metrics.set(metric_id::server_active_projects, 0);

    logger.info(log_component::server, operation, "server shutdown completed");
}

operation_id server_context::next_operation_id() noexcept {
    return operation_id{next_operation_value++};
}

} // namespace cw::server
