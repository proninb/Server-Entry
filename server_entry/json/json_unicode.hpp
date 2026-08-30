#pragma once

#include "json_buffer.hpp"

#include <cstdint>
#include <string_view>

namespace cw::server
{

[[nodiscard]] inline bool json_append_utf8(json_buffer& output, std::uint32_t code_point) noexcept
{
    if (code_point <= 0x7f) return output.append(static_cast<char>(code_point));
    if (code_point <= 0x7ff)
    {
        return output.append(static_cast<char>(0xc0 | (code_point >> 6))) &&
               output.append(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    if (code_point >= 0xd800 && code_point <= 0xdfff) return false;
    if (code_point <= 0xffff)
    {
        return output.append(static_cast<char>(0xe0 | (code_point >> 12))) &&
               output.append(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f))) &&
               output.append(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    if (code_point <= 0x10ffff)
    {
        return output.append(static_cast<char>(0xf0 | (code_point >> 18))) &&
               output.append(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f))) &&
               output.append(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f))) &&
               output.append(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    return false;
}

[[nodiscard]] inline bool json_valid_utf8(std::string_view input) noexcept
{
    for (std::size_t index = 0; index < input.size();)
    {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first <= 0x7f) { ++index; continue; }

        std::size_t count = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 2; value = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 3; value = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 4; value = first & 0x07; minimum = 0x10000; }
        else return false;

        if (index + count > input.size()) return false;
        for (std::size_t continuation = 1; continuation < count; ++continuation)
        {
            const auto byte = static_cast<unsigned char>(input[index + continuation]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
        index += count;
    }
    return true;
}

} // namespace cw::server
