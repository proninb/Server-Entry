#pragma once

#include <cstddef>
#include <cstdint>

namespace cw::server
{

enum class json_error_code : std::uint16_t
{
    none = 0,
    unexpected_end,
    unexpected_token,
    expected_object_key,
    expected_colon,
    expected_comma_or_end,
    invalid_string,
    invalid_escape,
    invalid_unicode,
    invalid_number,
    trailing_characters,
    allocation_failed,
};

struct json_error
{
    json_error_code code = json_error_code::none;
    std::size_t offset = 0;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return code == json_error_code::none;
    }
};

} // namespace cw::server
