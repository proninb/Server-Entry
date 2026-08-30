#pragma once

#include "diagnostic_descriptor.hpp"

#include <array>
#include <span>

namespace cw::server
{

class diagnostic_registry_view
{
public:
    constexpr explicit diagnostic_registry_view(
        std::span<const diagnostic_descriptor> descriptors) noexcept
        : descriptors_(descriptors)
    {
    }

    [[nodiscard]] constexpr const diagnostic_descriptor* find(diagnostic_id id) const noexcept
    {
        for (const auto& descriptor : descriptors_)
        {
            if (descriptor.id == id)
            {
                return &descriptor;
            }
        }
        return nullptr;
    }

private:
    std::span<const diagnostic_descriptor> descriptors_;
};

inline constexpr std::array diagnostic_descriptors{
    diagnostics::server_initialization_failed,
    diagnostics::server_invalid_json,
    diagnostics::server_invalid_configuration,
    diagnostics::server_unsupported_configuration_version,
    diagnostics::server_configuration_read_failed,
    diagnostics::project_initialization_failed,
    diagnostics::source_initialization_failed,
    diagnostics::runtime_attach_failed,
    diagnostics::shm_initialization_failed,
};

inline constexpr diagnostic_registry_view diagnostic_registry{diagnostic_descriptors};

} // namespace cw::server
