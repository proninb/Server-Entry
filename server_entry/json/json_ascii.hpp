#pragma once

namespace cw::server
{

[[nodiscard]] constexpr bool json_is_whitespace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] constexpr bool json_is_digit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr int json_hex_value(char value) noexcept
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

} // namespace cw::server
