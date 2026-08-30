#pragma once

#include "json_buffer.hpp"

#include <string_view>

namespace cw::server
{

[[nodiscard]] inline bool json_append_escaped(json_buffer& output, std::string_view value) noexcept
{
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': if (!output.append("\\\"")) return false; break;
        case '\\': if (!output.append("\\\\")) return false; break;
        case '\b': if (!output.append("\\b")) return false; break;
        case '\f': if (!output.append("\\f")) return false; break;
        case '\n': if (!output.append("\\n")) return false; break;
        case '\r': if (!output.append("\\r")) return false; break;
        case '\t': if (!output.append("\\t")) return false; break;
        default:
            if (character < 0x20)
            {
                char escaped[]{'\\', 'u', '0', '0', hex[character >> 4], hex[character & 0x0f]};
                if (!output.append({escaped, sizeof(escaped)})) return false;
            }
            else if (!output.append(static_cast<char>(character))) return false;
        }
    }
    return true;
}

} // namespace cw::server
