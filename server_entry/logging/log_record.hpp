#pragma once

#include "../operation.hpp"
#include "log.hpp"

#include <chrono>
#include <string>

namespace cw::server {

// Represents one fully constructed log event passed from logger to log sinks.
// log_record owns the message text and carries timestamp, severity, component,
// and operation correlation without retaining logger or subsystem state.
struct log_record {
    std::chrono::system_clock::time_point timestamp;
    log_level level = log_level::info;
    log_component component = log_component::server;
    operation_id operation;
    std::string message;
};

} // namespace cw::server