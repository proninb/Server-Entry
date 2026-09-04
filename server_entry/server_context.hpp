#pragma once

#include "operation.hpp"
#include "config/server_configuration.hpp"
#include "logging/console_log_sink.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_store.hpp"
#include "project/project_context.hpp"
#include "status.hpp"

namespace cw::server {

// Owns the top-level lifetime and shared services of one Server process.
// server_context initializes and shuts down the Server infrastructure, owns the
// project_context for the active project, and provides process-wide logging,
// metrics, configuration, and operation ID generation.
class server_context {
public:
    server_context();
    explicit server_context(server_configuration configuration);

    [[nodiscard]] status initialize() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] operation_id next_operation_id() noexcept;

private:
    const server_configuration config;
    std::uint64_t next_operation_value = 1;
    console_log_sink console_sink;
    logger logger;
    metrics_store metrics;
    project_context project;
};

} // namespace cw::server