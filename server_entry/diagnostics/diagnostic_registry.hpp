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
    diagnostics::project_invalid_json,
    diagnostics::project_invalid_configuration,
    diagnostics::project_unsupported_configuration_version,
    diagnostics::project_configuration_read_failed,
    diagnostics::project_composition_cycle,
    diagnostics::project_composition_failed,
    diagnostics::source_initialization_failed,
    diagnostics::runtime_attach_failed,
    diagnostics::shm_initialization_failed,
};

inline constexpr diagnostic_registry_view diagnostic_registry{diagnostic_descriptors};

consteval bool diagnostic_ids_unique()
{
    for (std::size_t left = 0; left < diagnostic_descriptors.size(); ++left)
    {
        for (std::size_t right = left + 1; right < diagnostic_descriptors.size(); ++right)
        {
            if (diagnostic_descriptors[left].id == diagnostic_descriptors[right].id)
            {
                return false;
            }
        }
    }
    return true;
}

consteval bool diagnostic_names_unique()
{
    for (std::size_t left = 0; left < diagnostic_descriptors.size(); ++left)
    {
        for (std::size_t right = left + 1; right < diagnostic_descriptors.size(); ++right)
        {
            if (diagnostic_descriptors[left].name == diagnostic_descriptors[right].name)
            {
                return false;
            }
        }
    }
    return true;
}

consteval bool diagnostic_descriptors_valid()
{
    for (const auto& descriptor : diagnostic_descriptors)
    {
        if (!descriptor.id || descriptor.domain == diagnostic_domain::unknown ||
            descriptor.name.empty() || descriptor.message.empty())
        {
            return false;
        }
    }
    return true;
}

static_assert(diagnostic_ids_unique());
static_assert(diagnostic_names_unique());
static_assert(diagnostic_descriptors_valid());

} // namespace cw::server
