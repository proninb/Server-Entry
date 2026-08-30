#pragma once

#include "../operation.hpp"
#include "log.hpp"

#include <chrono>
#include <string>

namespace cw::server
{

struct log_record
{
    std::chrono::system_clock::time_point timestamp;
    log_level level = log_level::info;
    log_component component = log_component::server;
    operation_id operation;
    std::string message;
};

} // namespace cw::server
