#pragma once

#include <cstdint>

namespace cw::server {

// Defines the ordered severity levels used by logger filtering and log sinks.
// The declaration order is significant because logger compares levels directly.
enum class log_level : std::uint8_t {
    trace,
    debug,
    info,
    warning,
    error,
    critical,
};

// Identifies the Server subsystem that produced a log event.
// Components provide architectural context for filtering, formatting, and diagnostics.
enum class log_component : std::uint8_t {
    server,
    project,
    source,
    runtime,
    shm,
    communication,
};

} // namespace cw::server