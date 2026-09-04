#include "console_log_sink.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace cw::server {
namespace {

constexpr std::string_view level_name(log_level level) noexcept {
    switch (level) {
    case log_level::trace:
        return "TRACE";
    case log_level::debug:
        return "DEBUG";
    case log_level::info:
        return "INFO";
    case log_level::warning:
        return "WARNING";
    case log_level::error:
        return "ERROR";
    case log_level::critical:
        return "CRITICAL";
    }

    return "INFO";
}

constexpr std::string_view component_name(log_component component) noexcept {
    switch (component) {
    case log_component::server:
        return "server";
    case log_component::project:
        return "project";
    case log_component::source:
        return "source";
    case log_component::runtime:
        return "runtime";
    case log_component::shm:
        return "shm";
    case log_component::communication:
        return "communication";
    }

    return "server";
}

} // namespace

void console_log_sink::write(const log_record& record) noexcept {
    try {
        const auto time = std::chrono::system_clock::to_time_t(record.timestamp);
        std::tm utc{};
        gmtime_s(&utc, &time);

        std::clog << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ") << ' '
                  << level_name(record.level) << ' '
                  << component_name(record.component)
                  << " op=" << record.operation.value() << ' '
                  << record.message << '\n';
    }
    catch (...) {
    }
}

} // namespace cw::server
