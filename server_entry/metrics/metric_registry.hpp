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
    {metric_id::source_acquisition_count, metric_kind::counter,
     "source.acquisition.count", "Number of physical source acquisition attempts"},
    {metric_id::source_acquisition_duration, metric_kind::duration,
     "source.acquisition.duration", "Total duration of physical source acquisition attempts"},
    {metric_id::source_file_open_duration, metric_kind::duration,
     "source.acquisition.open.duration", "Duration of opening source files"},
    {metric_id::source_observe_before_duration, metric_kind::duration,
     "source.acquisition.observe_before.duration", "Duration of source observation before reading"},
    {metric_id::source_read_duration, metric_kind::duration,
     "source.acquisition.read.duration", "Duration of reading source content"},
    {metric_id::source_observe_after_duration, metric_kind::duration,
     "source.acquisition.observe_after.duration", "Duration of source observation after reading"},
    {metric_id::source_sha256_duration, metric_kind::duration,
     "source.acquisition.sha256.duration", "Duration of source SHA-256 computation"},
    {metric_id::source_candidate_update_duration, metric_kind::duration,
     "source.acquisition.candidate_update.duration", "Duration of candidate physical-state update"},
    {metric_id::source_unchanged_fast_path_count, metric_kind::counter,
     "source.acquisition.unchanged_fast_path.count", "Number of acquisitions completed by equal-observation fast path"},
    {metric_id::source_content_read_count, metric_kind::counter,
     "source.acquisition.content_read.count", "Number of source contents read, including empty files"},
    {metric_id::source_candidate_added_count, metric_kind::counter,
     "source.candidate.added.count", "Number of final added source candidates observed"},
    {metric_id::source_candidate_modified_count, metric_kind::counter,
     "source.candidate.modified.count", "Number of final modified source candidates observed"},
    {metric_id::source_candidate_removed_count, metric_kind::counter,
     "source.candidate.removed.count", "Number of final removed source candidates observed"},
    {metric_id::source_candidate_observation_only_count, metric_kind::counter,
     "source.candidate.observation_only.count", "Number of final observation-only source candidates observed"},
    {metric_id::source_acquisition_failure_count, metric_kind::counter,
     "source.acquisition.failure.count", "Number of failed physical source acquisitions"},
    {metric_id::source_toctou_rejection_count, metric_kind::counter,
     "source.acquisition.toctou_rejection.count", "Number of acquisitions rejected after observation changed while reading"},
    {metric_id::source_bytes_read, metric_kind::counter,
     "source.acquisition.bytes_read", "Exact number of source bytes read"},
    {metric_id::source_checkpoint_save_attempt_count, metric_kind::counter, "source.checkpoint.save.attempt.count", "Source checkpoint save attempts"},
    {metric_id::source_checkpoint_save_success_count, metric_kind::counter, "source.checkpoint.save.success.count", "Successful source checkpoint saves"},
    {metric_id::source_checkpoint_save_failure_count, metric_kind::counter, "source.checkpoint.save.failure.count", "Failed source checkpoint saves"},
    {metric_id::source_checkpoint_source_count, metric_kind::counter, "source.checkpoint.source.count", "Sources serialized into checkpoints"},
    {metric_id::source_checkpoint_root_count, metric_kind::counter, "source.checkpoint.root.count", "Roots serialized into checkpoints"},
    {metric_id::source_checkpoint_forward_edge_count, metric_kind::counter, "source.checkpoint.forward_edge.count", "Forward dependency edges serialized"},
    {metric_id::source_checkpoint_reverse_edge_count, metric_kind::counter, "source.checkpoint.reverse_edge.count", "Reverse dependency edges serialized"},
    {metric_id::source_checkpoint_path_bytes, metric_kind::counter, "source.checkpoint.path.bytes", "WTF-8 path bytes serialized"},
    {metric_id::source_checkpoint_path_index_capacity, metric_kind::counter, "source.checkpoint.path_index.capacity", "Path index buckets serialized"},
    {metric_id::source_checkpoint_artifact_bytes, metric_kind::counter, "source.checkpoint.artifact.bytes", "Checkpoint artifact bytes produced"},
    {metric_id::source_checkpoint_bytes_hashed, metric_kind::counter, "source.checkpoint.hashed.bytes", "Checkpoint payload bytes hashed"},
    {metric_id::source_checkpoint_bytes_written, metric_kind::counter, "source.checkpoint.written.bytes", "Temporary checkpoint bytes written"},
    {metric_id::source_checkpoint_save_duration, metric_kind::duration, "source.checkpoint.save.duration", "Complete checkpoint save duration"},
    {metric_id::source_checkpoint_snapshot_layout_duration, metric_kind::duration, "source.checkpoint.snapshot_layout.duration", "Checkpoint snapshot and layout preparation duration"},
    {metric_id::source_checkpoint_path_materialization_duration, metric_kind::duration, "source.checkpoint.path_materialization.duration", "WTF-8 path materialization duration"},
    {metric_id::source_checkpoint_path_index_duration, metric_kind::duration, "source.checkpoint.path_index.duration", "Path index construction duration"},
    {metric_id::source_checkpoint_source_state_duration, metric_kind::duration, "source.checkpoint.source_state.duration", "SourceCore and PhysicalState materialization duration"},
    {metric_id::source_checkpoint_forward_csr_duration, metric_kind::duration, "source.checkpoint.forward_csr.duration", "Forward CSR materialization duration"},
    {metric_id::source_checkpoint_reverse_csr_duration, metric_kind::duration, "source.checkpoint.reverse_csr.duration", "Reverse CSR materialization duration"},
    {metric_id::source_checkpoint_payload_sha256_duration, metric_kind::duration, "source.checkpoint.payload_sha256.duration", "Checkpoint payload SHA-256 duration"},
    {metric_id::source_checkpoint_write_duration, metric_kind::duration, "source.checkpoint.write.duration", "Temporary checkpoint write duration"},
    {metric_id::source_checkpoint_flush_duration, metric_kind::duration, "source.checkpoint.flush.duration", "Checkpoint FlushFileBuffers duration"},
    {metric_id::source_checkpoint_reopen_map_duration, metric_kind::duration, "source.checkpoint.reopen_map.duration", "Checkpoint validation reopen and map duration"},
    {metric_id::source_checkpoint_artifact_validation_duration, metric_kind::duration, "source.checkpoint.artifact_validation.duration", "Checkpoint artifact validation duration"},
    {metric_id::source_checkpoint_deep_validation_duration, metric_kind::duration, "source.checkpoint.deep_validation.duration", "Checkpoint deep semantic validation duration"},
    {metric_id::compiled_save_total_duration, metric_kind::duration, "compiled.save.total", "Compiled checkpoint save duration"},
    {metric_id::compiled_save_materialization_duration, metric_kind::duration, "compiled.save.materialization", "Compiled checkpoint materialization duration"},
    {metric_id::compiled_save_hash_duration, metric_kind::duration, "compiled.save.hash", "Compiled checkpoint hashing duration"},
    {metric_id::compiled_save_write_duration, metric_kind::duration, "compiled.save.write", "Compiled checkpoint write duration"},
    {metric_id::compiled_save_flush_duration, metric_kind::duration, "compiled.save.flush", "Compiled checkpoint flush duration"},
    {metric_id::compiled_load_total_duration, metric_kind::duration, "compiled.load.total", "Compiled checkpoint load duration"},
    {metric_id::compiled_load_open_duration, metric_kind::duration, "compiled.load.open", "Compiled checkpoint open/read duration"},
    {metric_id::compiled_load_validate_duration, metric_kind::duration, "compiled.load.validate", "Compiled checkpoint validation duration"},
    {metric_id::compiled_load_strings_duration, metric_kind::duration, "compiled.load.strings", "String registry reconstruction duration"},
    {metric_id::compiled_load_graph_duration, metric_kind::duration, "compiled.load.graph", "Graph reconstruction duration"},
    {metric_id::compiled_load_publish_duration, metric_kind::duration, "compiled.load.publish", "Compiled candidate publication duration"},
    {metric_id::compiled_artifact_bytes, metric_kind::counter, "compiled.artifact.bytes", "Compiled artifact bytes"},
    {metric_id::compiled_string_count, metric_kind::counter, "compiled.string.count", "Compiled strings"},
    {metric_id::compiled_entity_slot_count, metric_kind::counter, "compiled.entity_slot.count", "Compiled entity slots"},
    {metric_id::compiled_type_slot_count, metric_kind::counter, "compiled.type_slot.count", "Compiled type slots"},
    {metric_id::compiled_enum_value_count, metric_kind::counter, "compiled.enum_value.count", "Compiled enum values"},
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
