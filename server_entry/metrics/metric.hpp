#pragma once

#include <cstddef>
#include <cstdint>

namespace cw::server
{

enum class metrics_mode : std::uint8_t
{
    off,
    basic,
    detailed,
};

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
    source_acquisition_count,
    source_acquisition_duration,
    source_file_open_duration,
    source_observe_before_duration,
    source_read_duration,
    source_observe_after_duration,
    source_sha256_duration,
    source_candidate_update_duration,
    source_unchanged_fast_path_count,
    source_content_read_count,
    source_candidate_added_count,
    source_candidate_modified_count,
    source_candidate_removed_count,
    source_candidate_observation_only_count,
    source_acquisition_failure_count,
    source_toctou_rejection_count,
    source_bytes_read,
    source_checkpoint_save_attempt_count,
    source_checkpoint_save_success_count,
    source_checkpoint_save_failure_count,
    source_checkpoint_source_count,
    source_checkpoint_root_count,
    source_checkpoint_forward_edge_count,
    source_checkpoint_reverse_edge_count,
    source_checkpoint_path_bytes,
    source_checkpoint_path_index_capacity,
    source_checkpoint_artifact_bytes,
    source_checkpoint_bytes_hashed,
    source_checkpoint_bytes_written,
    source_checkpoint_save_duration,
    source_checkpoint_snapshot_layout_duration,
    source_checkpoint_path_materialization_duration,
    source_checkpoint_path_index_duration,
    source_checkpoint_source_state_duration,
    source_checkpoint_forward_csr_duration,
    source_checkpoint_reverse_csr_duration,
    source_checkpoint_payload_sha256_duration,
    source_checkpoint_write_duration,
    source_checkpoint_flush_duration,
    source_checkpoint_reopen_map_duration,
    source_checkpoint_artifact_validation_duration,
    source_checkpoint_deep_validation_duration,
    compiled_save_total_duration,
    compiled_save_materialization_duration,
    compiled_save_hash_duration,
    compiled_save_write_duration,
    compiled_save_flush_duration,
    compiled_load_total_duration,
    compiled_load_open_duration,
    compiled_load_validate_duration,
    compiled_load_strings_duration,
    compiled_load_graph_duration,
    compiled_load_publish_duration,
    compiled_artifact_bytes,
    compiled_string_count,
    compiled_entity_slot_count,
    compiled_type_slot_count,
    compiled_enum_value_count,
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
