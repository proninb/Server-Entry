#pragma once

#include <cstdint>

namespace cw::server
{
class graph;
class graph_update;

class type_handle final
{
public:
    // The slot is meaningful only with the committed Graph generation from
    // which it was obtained; it is not cross-BUILD identity.
    constexpr type_handle() noexcept = default;
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return slot_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return slot_ != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }
    friend constexpr bool operator==(type_handle, type_handle) noexcept = default;

private:
    explicit constexpr type_handle(std::uint32_t slot) noexcept : slot_(slot) {}
    std::uint32_t slot_ = 0;
    friend class graph;
    friend class graph_update;
};
} // namespace cw::server
