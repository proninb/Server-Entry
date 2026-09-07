#pragma once

#include <cstdint>
#include <string_view>

namespace cw::server {

// Construction-only acceleration hash for the String Binding boundary.
// Hash equality is never canonical identity; byte equality remains authoritative.
[[nodiscard]] inline std::uint64_t string_binding_hash(
    std::string_view value) noexcept {

    std::uint64_t hash =
        1469598103934665603ull;

    for (const auto byte : value) {
        hash ^=
            static_cast<std::uint8_t>(
                byte);

        hash *=
            1099511628211ull;
    }

    hash ^= hash >> 32;
    hash *= 0xd6e8feb86659fd93ull;
    hash ^= hash >> 32;
    return hash;
}

} // namespace cw::server