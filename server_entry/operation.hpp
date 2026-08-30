#pragma once

#include <cstdint>

namespace cw::server
{

class operation_id
{
public:
    constexpr operation_id() noexcept = default;

    explicit constexpr operation_id(std::uint64_t value) noexcept
        : value_(value)
    {
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept
    {
        return value_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return value_ != 0;
    }

    friend constexpr bool operator==(operation_id, operation_id) noexcept = default;

private:
    std::uint64_t value_ = 0;
};

} // namespace cw::server
