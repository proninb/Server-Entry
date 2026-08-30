#pragma once

#include <cstdint>

namespace cw::server
{

enum class log_level : std::uint8_t
{
    trace,
    debug,
    info,
    warning,
    error,
    critical,
};

enum class log_component : std::uint8_t
{
    server,
    project,
    source,
    runtime,
    shm,
    communication,
};

} // namespace cw::server
