#pragma once

#include <cstdint>

namespace cw::server
{

class string_registry;
class string_registry_update;
class graph;

class string_id final
{
public:
    constexpr string_id() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(string_id, string_id) noexcept = default;

private:
    explicit constexpr string_id(std::uint32_t value) noexcept : value_(value) {}

    std::uint32_t value_ = 0;
    friend class string_registry;
    friend class string_registry_update;
    friend class graph;
};

} // namespace cw::server
