#include "builtin_type.hpp"

namespace cw::server
{
std::uint8_t builtin_bit_width(builtin_type type, const abi_configuration&) noexcept
{
    switch (type)
    {
    case builtin_type::boolean: return 8;
    case builtin_type::character:
    case builtin_type::signed_character:
    case builtin_type::unsigned_character:
    case builtin_type::character_8: return 8;
    case builtin_type::short_integer:
    case builtin_type::unsigned_short_integer:
    case builtin_type::wide_character:
    case builtin_type::character_16: return 16;
    case builtin_type::integer:
    case builtin_type::unsigned_integer:
    case builtin_type::long_integer:
    case builtin_type::unsigned_long_integer:
    case builtin_type::character_32:
    case builtin_type::floating: return 32;
    case builtin_type::long_long_integer:
    case builtin_type::unsigned_long_long_integer:
    case builtin_type::double_floating:
    case builtin_type::long_double_floating: return 64;
    }
    return 0;
}

bool builtin_is_signed(builtin_type type, const abi_configuration& abi) noexcept
{
    if (type == builtin_type::character)
        return abi.target == abi_target::windows_x64;
    return is_signed(type);
}

status select_unscoped_enum_underlying(std::span<const integral_constant> values,
                                       const abi_configuration& abi,
                                       builtin_type& output) noexcept
{
    return select_unscoped_enum_underlying_projected(
        values, abi, output, [](integral_constant value) noexcept { return value; });
}
} // namespace cw::server
