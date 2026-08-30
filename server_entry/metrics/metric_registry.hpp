#pragma once

#include "metric_descriptor.hpp"

#include <array>

namespace cw::server
{

inline constexpr std::array<metric_descriptor, metric_count> metric_descriptors{{
    {metric_id::server_initializations, metric_kind::counter,
     "server.initializations", "Number of server initialization attempts"},
    {metric_id::server_initialization_duration, metric_kind::duration,
     "server.initialization.duration", "Duration of server initialization attempts"},
    {metric_id::server_active_projects, metric_kind::gauge,
     "server.active_projects", "Current number of active projects"},
    {metric_id::project_initializations, metric_kind::counter,
     "project.initializations", "Number of project initialization attempts"},
    {metric_id::project_initialization_duration, metric_kind::duration,
     "project.initialization.duration", "Duration of project initialization attempts"},
    {metric_id::project_configuration_load_count, metric_kind::counter,
     "project.configuration.load.count", "Number of project configuration load attempts"},
    {metric_id::project_configuration_load_duration, metric_kind::duration,
     "project.configuration.load.duration", "Duration of project configuration load attempts"},
    {metric_id::project_configuration_parse_duration, metric_kind::duration,
     "project.configuration.parse.duration", "Duration of project configuration JSON parsing"},
    {metric_id::project_configuration_path_resolution_duration, metric_kind::duration,
     "project.configuration.path_resolution.duration", "Duration of project item path resolution"},
    {metric_id::project_configuration_item_count, metric_kind::counter,
     "project.configuration.item.count", "Number of successfully loaded project items"},
    {metric_id::project_composition_resolve_count, metric_kind::counter,
     "project.composition.resolve.count", "Number of project composition resolution attempts"},
    {metric_id::project_composition_resolve_duration, metric_kind::duration,
     "project.composition.resolve.duration", "Duration of project composition resolution attempts"},
    {metric_id::project_composition_file_count, metric_kind::counter,
     "project.composition.file.count", "Number of subproject configuration files loaded"},
    {metric_id::project_composition_cache_hit_count, metric_kind::counter,
     "project.composition.cache_hit.count", "Number of deduplicated project composition references"},
    {metric_id::project_composition_max_parallel_workers, metric_kind::gauge,
     "project.composition.max_parallel_workers", "Maximum active project composition workers in the latest resolution"},
    {metric_id::runtime_attach_count, metric_kind::counter,
     "runtime.attach.count", "Number of Runtime attachment attempts"},
    {metric_id::runtime_attach_duration, metric_kind::duration,
     "runtime.attach.duration", "Duration of Runtime attachment attempts"},
    {metric_id::shm_initializations, metric_kind::counter,
     "shm.initializations", "Number of Shared Memory initialization attempts"},
    {metric_id::shm_initialization_duration, metric_kind::duration,
     "shm.initialization.duration", "Duration of Shared Memory initialization attempts"},
}};

[[nodiscard]] constexpr const metric_descriptor& descriptor(metric_id id) noexcept
{
    return metric_descriptors[metric_index(id)];
}

consteval bool metric_registry_is_valid()
{
    if (metric_descriptors.size() != metric_count)
    {
        return false;
    }

    for (std::size_t index = 0; index < metric_descriptors.size(); ++index)
    {
        if (metric_index(metric_descriptors[index].id) != index)
        {
            return false;
        }
    }
    return true;
}

static_assert(metric_registry_is_valid());

} // namespace cw::server
