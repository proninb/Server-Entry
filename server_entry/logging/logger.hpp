#pragma once

#include "log_sink.hpp"

#include <atomic>
#include <mutex>
#include <string_view>
#include <vector>

namespace cw::server {

// Provides process-wide, thread-safe log fan-out to registered log sinks.
// logger owns only the sink registry, not the sinks themselves; registered sinks
// must outlive their registration. Logging is best-effort and never propagates
// sink or allocation failures back into Server control flow.
class logger {
public:
    void add_sink(log_sink& sink);
    void set_minimum_level(log_level level) noexcept;

    void write(
        log_level level,
        log_component component,
        operation_id operation,
        std::string_view message) noexcept;

    void info(
        log_component component,
        operation_id operation,
        std::string_view message) noexcept;

    void error(
        log_component component,
        operation_id operation,
        std::string_view message) noexcept;

private:
    std::atomic<log_level> minimum_level{log_level::info};
    std::mutex mutex;
    std::vector<log_sink*> sinks;
};

} // namespace cw::server
