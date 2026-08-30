#pragma once

#include <cstddef>
#include <cstdint>

namespace cw::server
{

enum class metric_id : std::uint16_t
{
    server_initializations = 0,
    server_initialization_duration,
    server_active_projects,
    project_initializations,
    project_initialization_duration,
    project_configuration_load_count,
    project_configuration_load_duration,
    project_configuration_parse_duration,
    project_configuration_path_resolution_duration,
    project_configuration_item_count,
    project_composition_resolve_count,
    project_composition_resolve_duration,
    project_composition_file_count,
    project_composition_cache_hit_count,
    project_composition_max_parallel_workers,
    runtime_attach_count,
    runtime_attach_duration,
    shm_initializations,
    shm_initialization_duration,
    count,
};

enum class metric_kind : std::uint8_t
{
    counter,
    gauge,
    duration,
};

[[nodiscard]] constexpr std::size_t metric_index(metric_id id) noexcept
{
    return static_cast<std::size_t>(id);
}

inline constexpr std::size_t metric_count = metric_index(metric_id::count);

} // namespace cw::server
