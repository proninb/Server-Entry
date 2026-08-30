#pragma once

#include "log_sink.hpp"

#include <atomic>
#include <mutex>
#include <string_view>
#include <vector>

namespace cw::server
{

class logger
{
public:
    void add_sink(log_sink& sink);
    void set_minimum_level(log_level level) noexcept;

    void write(log_level level, log_component component, operation_id operation,
               std::string_view message) noexcept;

    void info(log_component component, operation_id operation,
              std::string_view message) noexcept;
    void error(log_component component, operation_id operation,
               std::string_view message) noexcept;

private:
    std::atomic<log_level> minimum_level_{log_level::info};
    std::mutex mutex_;
    std::vector<log_sink*> sinks_;
};

} // namespace cw::server
