#pragma once

#include "operation.hpp"
#include "config/server_configuration.hpp"
#include "logging/console_log_sink.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_store.hpp"
#include "project/project_context.hpp"
#include "status.hpp"

namespace cw::server
{

class server_context
{
public:
    server_context();
    explicit server_context(server_configuration configuration);

    [[nodiscard]] status initialize() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] operation_id next_operation_id() noexcept;

private:
    const server_configuration config_;
    std::uint64_t next_operation_value_ = 1;
    console_log_sink console_sink_;
    logger logger_;
    metrics_store metrics_;
    project_context project_;
};

} // namespace cw::server
