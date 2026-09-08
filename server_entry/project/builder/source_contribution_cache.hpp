#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../graph/graph.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cw::server {

// One-based slice into source_contribution_state::enum_values.
// The coordinate is Source-local construction state and is never Runtime-visible.
struct source_definition_range {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return begin != 0;
    }
};

// One canonical declaration/definition contributed by one Source.
// Retained outside G only to compute sparse Source replacement deltas.
struct source_contribution_record {
    stable_id entity{};
    string_id name{};
    entity_kind kind = entity_kind::enum_type;
    enum_definition_state state = enum_definition_state::opaque;
    bool scoped = false;
    bool fixed = false;
    builtin_type underlying = builtin_type::integer;
    source_definition_range definition{};
};


// Build-side aggregation for one canonical named Entity. This state is derived
// from SourceContribution records and exists only to support sparse incremental
// remove/add materialization. It is never part of committed G.
struct canonical_entity_construction_state {
    std::uint32_t declarations = 0;
    std::uint32_t scoped = 0;
    std::uint32_t unscoped = 0;
    std::uint32_t fixed = 0;
    std::uint32_t nonfixed = 0;
    std::uint32_t definitions = 0;
    std::uint32_t aggregate_declarations = 0;
    std::uint32_t aggregate_definitions = 0;

    // All active fixed declarations for one Entity must agree on one type.
    builtin_type active_type = builtin_type::integer;
    builtin_type definition_type = builtin_type::integer;
    source_id definition_source{};
    source_definition_range definition{};
    source_id aggregate_definition_source{};
};

// Complete incremental build contribution retained for one source_id.
struct source_contribution_state {
    // Direct source_declaration_id -> generation-local canonical type coordinate.
    // G0 rebuilds this cache; incremental builds preserve type_handle identity.
    std::vector<type_handle> type_bindings;

    std::vector<source_contribution_record> named;
    std::vector<std::uint32_t> anonymous_types;

    // All enum definitions contributed by this Source share one dense arena.
    // Individual source_contribution_record values address it by range.
    std::vector<enum_value_record> enum_values;
};

// Non-authoritative incremental build cache keyed by source_id.
// It remembers the previous canonical contribution of each Source so an
// incremental replacement can remove only that Source's old contribution.
// The cache is never Runtime-visible and is intentionally excluded from G.
class source_contribution_cache_update;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

struct source_contribution_storage_snapshot {
    const void* states_data = nullptr;
    const void* entity_states_data = nullptr;
    std::size_t states_size = 0;
    std::size_t entity_states_size = 0;
    std::size_t states_capacity = 0;
    std::size_t entity_states_capacity = 0;
};

#endif

class source_contribution_cache final {
public:
    [[nodiscard]] status initialize() noexcept;
    [[nodiscard]] source_contribution_cache_update begin_update(bool full_reconstruction) noexcept;

    [[nodiscard]] std::size_t contribution_count(source_id source) const noexcept;
    [[nodiscard]] bool complete() const noexcept { return provenance_complete; }

    // Used after loading state that deliberately omits build provenance.
    void invalidate() noexcept;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    [[nodiscard]] source_contribution_storage_snapshot
        storage_snapshot_for_testing() const noexcept;
#endif

private:
    friend class source_contribution_cache_update;

    struct candidate_slot {
        std::uint64_t generation = 0;
        source_contribution_state value;
        bool previous_retained = false;
    };

    struct candidate_entity_slot {
        std::uint64_t generation = 0;
        canonical_entity_construction_state value;
    };

    std::vector<source_contribution_state> states;
    std::vector<candidate_slot> candidates;
    std::vector<canonical_entity_construction_state> entity_states;
    std::vector<candidate_entity_slot> candidate_entities;
    std::uint64_t next_candidate_generation = 1;
    bool provenance_complete = true;
};

// Sparse candidate mutation of SourceContribution[source_id].
// Only Source IDs replaced by the current transaction receive candidate state.
class source_contribution_cache_update final {
public:
    source_contribution_cache_update() noexcept = default;
    ~source_contribution_cache_update() = default;

    source_contribution_cache_update(const source_contribution_cache_update&) = delete;
    source_contribution_cache_update& operator=(const source_contribution_cache_update&) = delete;

    source_contribution_cache_update(source_contribution_cache_update&& other) noexcept;
    source_contribution_cache_update& operator=(source_contribution_cache_update&&) = delete;

    [[nodiscard]] bool was_replaced(source_id source) const noexcept;
    [[nodiscard]] const source_contribution_state* committed(source_id source) const noexcept;

    [[nodiscard]] const source_contribution_state* retained_previous(
        source_id source) const noexcept;

    [[nodiscard]] status retain_previous(
        source_id source) noexcept;

    void release_previous(source_id source) noexcept;

    // G0 bulk reservation for reusable generation-local cache overlays.
    [[nodiscard]] status reserve_rebuild(
        std::size_t source_slots,
        std::size_t entity_slots) noexcept;
    // Starts a new empty contribution for Source. Replacing a Source means its
    // old committed contribution is removed by Graph and this candidate is then
    // filled with the Source's new canonical contribution.
    [[nodiscard]] status replace(
        source_id source,
        source_contribution_state*& output) noexcept;

    [[nodiscard]] source_contribution_state* candidate(source_id source) noexcept;
    [[nodiscard]] const source_contribution_state* candidate(source_id source) const noexcept;

    [[nodiscard]] canonical_entity_construction_state& touch_entity(stable_id entity);
    [[nodiscard]] const canonical_entity_construction_state* candidate_entity(stable_id entity) const noexcept;

    [[nodiscard]] const std::vector<std::uint32_t>& changed_sources() const noexcept {
        return changed;
    }

    [[nodiscard]] status prepare_publish() noexcept;
    void publish_prepared() noexcept;
    void cancel() noexcept;

private:
    friend class source_contribution_cache;

    source_contribution_cache_update(
        source_contribution_cache& owner,
        std::uint64_t candidate_generation,
        bool full_reconstruction) noexcept;

    source_contribution_cache* owner = nullptr;
    std::vector<std::uint32_t> changed;
    std::vector<std::uint32_t> changed_entities;
    std::uint64_t candidate_generation = 0;
    bool full_reconstruction = false;
    bool prepared = false;
    bool committed_update = false;
    std::size_t prepared_states_size = 0;
    std::size_t prepared_entity_states_size = 0;
    bool prepared_owner_growth = false;
    status failure{};
};

} // namespace cw::server
