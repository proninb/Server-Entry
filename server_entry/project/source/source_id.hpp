#pragma once

#include <cstdint>

namespace cw::server
{

class source_id
{
public:
    constexpr source_id() noexcept = default;

    explicit constexpr source_id(std::uint32_t value) noexcept
        : value_(value)
    {
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept
    {
        return value_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return value_ != 0;
    }

    friend constexpr bool operator==(source_id, source_id) noexcept = default;

private:
    std::uint32_t value_ = 0;
};

} // namespace cw::server
