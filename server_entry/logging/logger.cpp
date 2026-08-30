#include "logger.hpp"

#include <chrono>

namespace cw::server
{

void logger::add_sink(log_sink& sink)
{
    std::scoped_lock lock{mutex_};
    sinks_.push_back(&sink);
}

void logger::set_minimum_level(log_level level) noexcept
{
    minimum_level_.store(level, std::memory_order_relaxed);
}

void logger::write(log_level level, log_component component, operation_id operation,
                   std::string_view message) noexcept
{
    if (level < minimum_level_.load(std::memory_order_relaxed))
    {
        return;
    }
    try
    {
        log_record record{std::chrono::system_clock::now(), level, component, operation,
                          std::string{message}};
        std::scoped_lock lock{mutex_};
        for (auto* sink : sinks_)
        {
            sink->write(record);
        }
    }
    catch (...)
    {
    }
}

void logger::info(log_component component, operation_id operation,
                  std::string_view message) noexcept
{
    write(log_level::info, component, operation, message);
}

void logger::error(log_component component, operation_id operation,
                   std::string_view message) noexcept
{
    write(log_level::error, component, operation, message);
}

} // namespace cw::server
