#pragma once

#include "../project_configuration.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>

namespace cw::server
{
enum class builtin_type : std::uint8_t
{
    boolean,
    character,
    signed_character,
    unsigned_character,
    short_integer,
    unsigned_short_integer,
    integer,
    unsigned_integer,
    long_integer,
    unsigned_long_integer,
    long_long_integer,
    unsigned_long_long_integer,
    wide_character,
    character_8,
    character_16,
    character_32,
    floating,
    double_floating,
    long_double_floating,
    void_type
};

struct integral_constant
{
    builtin_type type = builtin_type::integer;
    std::uint64_t bits = 0;
};

[[nodiscard]] constexpr bool is_integral(builtin_type type) noexcept
{
    return type <= builtin_type::character_32;
}
[[nodiscard]] constexpr bool is_floating(builtin_type type) noexcept
{
    return type >= builtin_type::floating && type <= builtin_type::long_double_floating;
}
[[nodiscard]] constexpr bool is_signed(builtin_type type) noexcept
{
    switch (type)
    {
    case builtin_type::signed_character:
    case builtin_type::short_integer:
    case builtin_type::integer:
    case builtin_type::long_integer:
    case builtin_type::long_long_integer: return true;
    default: return false;
    }
}

[[nodiscard]] std::uint8_t builtin_bit_width(
    builtin_type type, const abi_configuration& abi) noexcept;
[[nodiscard]] bool builtin_is_signed(
    builtin_type type, const abi_configuration& abi) noexcept;
[[nodiscard]] status select_unscoped_enum_underlying(
    std::span<const integral_constant> values, const abi_configuration& abi,
    builtin_type& output) noexcept;

template<class Range, class Projection>
[[nodiscard]] status select_unscoped_enum_underlying_projected(
    const Range& values, const abi_configuration& abi, builtin_type& output,
    Projection projection) noexcept
{
    if (abi.target != abi_target::windows_x64)
        return {status_code::configuration_failed};
    std::int64_t minimum = 0;
    std::uint64_t maximum = 0;
    bool has_negative = false;
    for (const auto& item : values)
    {
        const integral_constant value = std::invoke(projection, item);
        if (!is_integral(value.type)) return {status_code::configuration_failed};
        const auto width = builtin_bit_width(value.type, abi);
        if (builtin_is_signed(value.type, abi))
        {
            const auto signed_value = width == 64
                ? static_cast<std::int64_t>(value.bits)
                : static_cast<std::int64_t>(value.bits << (64 - width)) >> (64 - width);
            if (signed_value < 0)
            {
                has_negative = true;
                if (signed_value < minimum) minimum = signed_value;
            }
            else if (static_cast<std::uint64_t>(signed_value) > maximum)
                maximum = static_cast<std::uint64_t>(signed_value);
        }
        else if (value.bits > maximum) maximum = value.bits;
    }
    if (minimum >= std::numeric_limits<std::int32_t>::min() &&
        maximum <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        output = builtin_type::integer;
    else if (!has_negative && maximum <= std::numeric_limits<std::uint32_t>::max())
        output = builtin_type::unsigned_integer;
    else if (maximum <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        output = builtin_type::long_long_integer;
    else if (!has_negative) output = builtin_type::unsigned_long_long_integer;
    else return {status_code::configuration_failed};
    return {};
}
} // namespace cw::server
