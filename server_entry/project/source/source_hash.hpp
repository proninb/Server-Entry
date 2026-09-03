#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace cw::server
{
struct source_content_hash
{
    std::array<std::byte, 32> bytes{};
    friend constexpr bool operator==(const source_content_hash&,
                                     const source_content_hash&) noexcept = default;
};

[[nodiscard]] source_content_hash hash_source_content(std::string_view bytes) noexcept;
} // namespace cw::server
