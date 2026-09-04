#pragma once

#include <cstdint>

namespace cw::server {

// Identifies one member position inside an aggregate definition.
// The index is owner-relative and has no meaning without its aggregate.
class member_index final {
public:
    constexpr member_index() noexcept = default;
    explicit constexpr member_index(std::uint32_t value) noexcept : index(value) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return index; }
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(member_index, member_index) noexcept = default;

private:
    std::uint32_t index = 0;
};

} // namespace cw::server
