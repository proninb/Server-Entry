#pragma once

#include "../project/graph/graph.hpp"
#include "../project/builder/source_contribution_cache.hpp"

namespace cw::server::rc_v2_01a {

[[nodiscard]] inline bool graph_reallocated(
    const graph_storage_prepare_telemetry& value) noexcept {

    return
        value.identity.reallocated ||
        value.entities.reallocated ||
        value.types.reallocated ||
        value.member_records.reallocated ||
        value.enum_value_records.reallocated ||
        value.canonical_types.reallocated ||
        value.named_type_refs.reallocated;
}

[[nodiscard]] inline std::size_t graph_relocation_bytes(
    const graph_storage_prepare_telemetry& value) noexcept {

    return
        value.identity.relocation_payload_bytes +
        value.entities.relocation_payload_bytes +
        value.types.relocation_payload_bytes +
        value.member_records.relocation_payload_bytes +
        value.enum_value_records.relocation_payload_bytes +
        value.canonical_types.relocation_payload_bytes +
        value.named_type_refs.relocation_payload_bytes;
}

[[nodiscard]] inline bool contribution_reallocated(
    const source_contribution_storage_snapshot& before,
    const source_contribution_storage_snapshot& after) noexcept {

    return
        before.states_data != after.states_data ||
        before.entity_states_data != after.entity_states_data;
}

[[nodiscard]] inline std::size_t contribution_relocation_bytes(
    const source_contribution_storage_snapshot& before,
    const source_contribution_storage_snapshot& after) noexcept {

    std::size_t result = 0;

    if (before.states_data != after.states_data) {
        result +=
            before.states_size *
            sizeof(source_contribution_state);
    }

    if (before.entity_states_data !=
        after.entity_states_data) {
        result +=
            before.entity_states_size *
            sizeof(canonical_entity_construction_state);
    }

    return result;
}

[[nodiscard]] inline bool sparse_independent_gate(
    const graph_storage_prepare_telemetry& value) noexcept {

    return
        value.changed_sources == 1 &&
        value.changed_entities == 1 &&
        value.changed_types == 1 &&
        value.validation_visited_types == 1 &&
        value.validation_dependency_edges == 0 &&
        !graph_reallocated(value);
}

[[nodiscard]] inline bool dependency_chain_gate(
    const graph_storage_prepare_telemetry& value) noexcept {

    return
        value.changed_types == 1 &&
        value.validation_visited_types == 3 &&
        value.validation_dependency_edges == 2;
}

} // namespace cw::server::rc_v2_01a
