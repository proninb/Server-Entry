#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace cw::server::core
{
using sha256_digest = std::array<std::byte, 32>;
[[nodiscard]] sha256_digest sha256(std::string_view bytes) noexcept;
} // namespace cw::server::core
