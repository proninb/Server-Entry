#pragma once

#include <cstdint>

namespace cw::server
{

enum class status_code : std::uint32_t
{
    ok = 0,
    initialization_failed,
    configuration_failed,
    duplicate_source_replacement,
    invalid_state,
    persistence_failed,
    artifact_corrupt,
    not_available,
};

struct status
{
    status_code code = status_code::ok;

    [[nodiscard]]
    constexpr bool ok() const noexcept
    {
        return code == status_code::ok;
    }
};

} // namespace cw::server
