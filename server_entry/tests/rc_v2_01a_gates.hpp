#pragma once

#if !defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
#error "RC-V2-01A gates require CW_GRAPH_BUILD_TRANSACTION_TESTING"
#endif

#include "../project/graph/graph_build_transaction_test_access.hpp"

#include <cstddef>
#include <cstdint>
#include <ostream>

namespace cw::server::rc_v2_01a {

struct gate_result {
    bool passed = true;
    const char* reason = "pass";
};

[[nodiscard]] inline bool same_logical_vector(
    const graph_vector_storage_snapshot& before,
    const graph_vector_storage_snapshot& after) noexcept {

    return before.size == after.size;
}

// Capacity and data addresses are deliberately excluded: prepare is allowed to
// retain successful reserve() growth after a failed candidate publication.
[[nodiscard]] inline bool same_logical_graph_storage(
    const graph_storage_snapshot& before,
    const graph_storage_snapshot& after) noexcept {

    return
        same_logical_vector(before.identity, after.identity) &&
        same_logical_vector(before.entities, after.entities) &&
        same_logical_vector(before.enum_aggregates, after.enum_aggregates) &&
        same_logical_vector(before.source_states, after.source_states) &&
        same_logical_vector(before.types, after.types) &&
        same_logical_vector(before.free_type_slots, after.free_type_slots) &&
        same_logical_vector(before.member_records, after.member_records) &&
        same_logical_vector(before.canonical_types, after.canonical_types) &&
        same_logical_vector(before.named_type_refs, after.named_type_refs) &&
        before.derived_type_index.size == after.derived_type_index.size &&
        before.entity_count == after.entity_count &&
        before.user_type_count == after.user_type_count &&
        before.next_stable_id == after.next_stable_id &&
        before.generation == after.generation &&
        before.semantic_hash == after.semantic_hash;
}

[[nodiscard]] inline gate_result check_g0_headroom(
    const graph_storage_snapshot& state,
    bool require_member_headroom = true) noexcept {

    if (state.entities.size != 0 && state.entities.capacity <= state.entities.size) {
        return {false, "entities has no G0 spare capacity"};
    }

    if (state.enum_aggregates.size != 0 &&
        state.enum_aggregates.capacity <= state.enum_aggregates.size) {
        return {false, "enum_aggregates has no G0 spare capacity"};
    }

    if (state.identity.size != 0 && state.identity.capacity <= state.identity.size) {
        return {false, "identity has no G0 spare capacity"};
    }

    if (state.source_states.size != 0 &&
        state.source_states.capacity <= state.source_states.size) {
        return {false, "source_states has no G0 spare capacity"};
    }

    if (state.types.size != 0 && state.types.capacity <= state.types.size) {
        return {false, "types has no G0 spare capacity"};
    }

    if (require_member_headroom && state.member_records.size != 0 &&
        state.member_records.capacity <= state.member_records.size) {
        return {false, "member_records has no G0 spare capacity"};
    }

    if (state.canonical_types.size != 0 &&
        state.canonical_types.capacity <= state.canonical_types.size) {
        return {false, "canonical_types has no G0 spare capacity"};
    }

    return {};
}

[[nodiscard]] inline gate_result check_isolated_delta(
    const graph_storage_prepare_telemetry& telemetry,
    std::size_t expected_members,
    std::size_t expected_type_refs) noexcept {

    if (telemetry.changed_sources != 1) {
        return {false, "changed_sources != 1"};
    }

    if (telemetry.changed_entities != 1) {
        return {false, "changed_entities != 1"};
    }

    if (telemetry.changed_types != 1) {
        return {false, "changed_types != 1"};
    }

    if (telemetry.added_members != expected_members) {
        return {false, "added_members mismatch"};
    }

    if (telemetry.added_type_refs != expected_type_refs) {
        return {false, "added_type_refs mismatch"};
    }

    return {};
}

[[nodiscard]] inline gate_result check_no_project_sized_relocation(
    const graph_storage_prepare_telemetry& telemetry) noexcept {

    if (telemetry.entities.reallocated) {
        return {false, "entities reallocated during sparse delta"};
    }

    if (telemetry.enum_aggregates.reallocated) {
        return {false, "enum_aggregates reallocated during sparse delta"};
    }

    if (telemetry.types.reallocated) {
        return {false, "types reallocated during sparse delta"};
    }

    if (telemetry.member_records.reallocated) {
        return {false, "member_records reallocated during sparse delta"};
    }

    if (telemetry.canonical_types.reallocated) {
        return {false, "canonical_types reallocated during sparse delta"};
    }

    return {};
}

[[nodiscard]] inline gate_result check_stable_storage_when_within_headroom(
    const graph_storage_snapshot& before,
    const graph_storage_snapshot& after,
    bool identity_expected_stable,
    bool source_states_expected_stable,
    bool canonical_types_expected_stable) noexcept {

    if (before.entities.data != after.entities.data) {
        return {false, "entities data pointer changed"};
    }

    if (before.enum_aggregates.data != after.enum_aggregates.data) {
        return {false, "enum_aggregates data pointer changed"};
    }

    if (before.types.data != after.types.data) {
        return {false, "types data pointer changed"};
    }

    if (before.member_records.data != after.member_records.data) {
        return {false, "member_records data pointer changed"};
    }

    if (identity_expected_stable && before.identity.data != after.identity.data) {
        return {false, "identity data pointer changed inside reserved headroom"};
    }

    if (source_states_expected_stable &&
        before.source_states.data != after.source_states.data) {
        return {false, "source_states data pointer changed inside reserved headroom"};
    }

    if (canonical_types_expected_stable &&
        before.canonical_types.data != after.canonical_types.data) {
        return {false, "canonical_types data pointer changed inside reserved headroom"};
    }

    return {};
}

[[nodiscard]] inline gate_result check_fail_closed_after_graph_prepare(
    status commit_result,
    graph_build_transaction_state transaction_state,
    const graph_storage_snapshot& before,
    const graph_storage_snapshot& after) noexcept {

    if (commit_result.ok()) {
        return {false, "forced post-Graph-prepare failure unexpectedly committed"};
    }

    if (transaction_state != graph_build_transaction_state::failed) {
        return {false, "transaction did not enter failed state"};
    }

    if (!same_logical_graph_storage(before, after)) {
        return {false, "committed Graph logical state changed after failed prepare"};
    }

    return {};
}

template <typename ConfigureCandidate>
[[nodiscard]] inline gate_result run_fail_closed_after_graph_prepare_gate(
    graph_manager& manager,
    ConfigureCandidate&& configure_candidate) {

    const auto before =
        graph_build_transaction_test_access::storage_snapshot(manager);

    auto transaction = manager.begin_build();

    const auto configured = configure_candidate(transaction);

    if (!configured.ok()) {
        return {false, "candidate configuration failed before injection point"};
    }

    graph_build_transaction_test_access::set_fail_after_graph_prepare(transaction);

    const auto commit_result = transaction.commit();
    const auto transaction_state =
        graph_build_transaction_test_access::transaction_state(transaction);

    const auto after =
        graph_build_transaction_test_access::storage_snapshot(manager);

    return check_fail_closed_after_graph_prepare(
        commit_result,
        transaction_state,
        before,
        after);
}

[[nodiscard]] inline std::uint64_t storage_prepare_instrumented_ns(
    const graph_storage_prepare_telemetry& value) noexcept {

    return
        value.identity.prepare_ns +
        value.entities.prepare_ns +
        value.enum_aggregates.prepare_ns +
        value.source_states.prepare_ns +
        value.types.prepare_ns +
        value.free_type_slots.prepare_ns +
        value.member_records.prepare_ns +
        value.canonical_types.prepare_ns +
        value.named_type_refs.prepare_ns +
        value.derived_type_index.prepare_ns;
}

inline void write_storage_telemetry_csv_header(std::ostream& output) {
    output
        << ",graph_storage_changed_identities"
        << ",graph_storage_changed_entities"
        << ",graph_storage_changed_types"
        << ",graph_storage_changed_sources"
        << ",graph_storage_added_members"
        << ",graph_storage_added_type_refs"
        << ",identity_prepare_ns,identity_touched,identity_size_before,identity_size_after,identity_capacity_before,identity_capacity_after,identity_reallocated,identity_relocation_payload_bytes"
        << ",entities_prepare_ns,entities_touched,entities_size_before,entities_size_after,entities_capacity_before,entities_capacity_after,entities_reallocated,entities_relocation_payload_bytes"
        << ",enum_aggregates_prepare_ns,enum_aggregates_touched,enum_aggregates_size_before,enum_aggregates_size_after,enum_aggregates_capacity_before,enum_aggregates_capacity_after,enum_aggregates_reallocated,enum_aggregates_relocation_payload_bytes"
        << ",source_states_prepare_ns,source_states_touched,source_states_size_before,source_states_size_after,source_states_capacity_before,source_states_capacity_after,source_states_reallocated,source_states_relocation_payload_bytes"
        << ",types_prepare_ns,types_touched,types_size_before,types_size_after,types_capacity_before,types_capacity_after,types_reallocated,types_relocation_payload_bytes"
        << ",free_type_slots_prepare_ns,free_type_slots_touched,free_type_slots_size_before,free_type_slots_size_after,free_type_slots_capacity_before,free_type_slots_capacity_after,free_type_slots_reallocated,free_type_slots_relocation_payload_bytes"
        << ",member_records_prepare_ns,member_records_touched,member_records_size_before,member_records_size_after,member_records_capacity_before,member_records_capacity_after,member_records_reallocated,member_records_relocation_payload_bytes"
        << ",canonical_types_prepare_ns,canonical_types_touched,canonical_types_size_before,canonical_types_size_after,canonical_types_capacity_before,canonical_types_capacity_after,canonical_types_reallocated,canonical_types_relocation_payload_bytes"
        << ",named_type_refs_prepare_ns,named_type_refs_touched,named_type_refs_size_before,named_type_refs_size_after,named_type_refs_capacity_before,named_type_refs_capacity_after,named_type_refs_reallocated,named_type_refs_relocation_payload_bytes"
        << ",derived_type_index_prepare_ns,derived_type_index_touched,derived_type_index_size_before,derived_type_index_size_after,derived_type_index_buckets_before,derived_type_index_buckets_after,derived_type_index_rehashed";
}

inline void write_vector_telemetry_csv_values(
    std::ostream& output,
    const graph_vector_growth_telemetry& value) {

    output
        << ',' << value.prepare_ns
        << ',' << value.logical_entries_touched
        << ',' << value.size_before
        << ',' << value.size_after
        << ',' << value.capacity_before
        << ',' << value.capacity_after
        << ',' << (value.reallocated ? 1 : 0)
        << ',' << value.relocation_payload_bytes;
}

inline void write_storage_telemetry_csv_values(
    std::ostream& output,
    const graph_storage_prepare_telemetry& value) {

    output
        << ',' << value.changed_identities
        << ',' << value.changed_entities
        << ',' << value.changed_types
        << ',' << value.changed_sources
        << ',' << value.added_members
        << ',' << value.added_type_refs;

    write_vector_telemetry_csv_values(output, value.identity);
    write_vector_telemetry_csv_values(output, value.entities);
    write_vector_telemetry_csv_values(output, value.enum_aggregates);
    write_vector_telemetry_csv_values(output, value.source_states);
    write_vector_telemetry_csv_values(output, value.types);
    write_vector_telemetry_csv_values(output, value.free_type_slots);
    write_vector_telemetry_csv_values(output, value.member_records);
    write_vector_telemetry_csv_values(output, value.canonical_types);
    write_vector_telemetry_csv_values(output, value.named_type_refs);

    output
        << ',' << value.derived_type_index.prepare_ns
        << ',' << value.derived_type_index.logical_entries_touched
        << ',' << value.derived_type_index.size_before
        << ',' << value.derived_type_index.size_after
        << ',' << value.derived_type_index.buckets_before
        << ',' << value.derived_type_index.buckets_after
        << ',' << (value.derived_type_index.rehashed ? 1 : 0);
}

} // namespace cw::server::rc_v2_01a
