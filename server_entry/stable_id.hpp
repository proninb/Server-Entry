#pragma once

#include <cstdint>

namespace cw::server
{
class graph;
class graph_update;

class stable_id final
{
public:
    constexpr stable_id() noexcept = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != 0; }
    friend constexpr bool operator==(stable_id, stable_id) noexcept = default;

private:
    explicit constexpr stable_id(std::uint32_t value) noexcept : value_(value) {}
    std::uint32_t value_ = 0;
    friend class graph;
    friend class graph_update;
};
} // namespace cw::server
