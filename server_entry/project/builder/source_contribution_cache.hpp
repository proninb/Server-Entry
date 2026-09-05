#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../graph/graph.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace cw::server {

// Immutable payload shared by build-side Source contributions that provide one
// enum definition. This is construction/cache state, not committed G state.
struct source_definition_payload {
    builtin_type underlying = builtin_type::integer;
    std::vector<enum_value_record> values;
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
    std::shared_ptr<const source_definition_payload> definition;
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
    std::uint32_t active_fixed = 0;
    std::uint32_t aggregate_declarations = 0;
    std::uint32_t aggregate_definitions = 0;

    std::array<std::uint32_t, static_cast<std::size_t>(builtin_type::void_type) + 1> underlying{};

    builtin_type active_type = builtin_type::integer;
    source_id definition_source{};
    std::shared_ptr<const source_definition_payload> definition;
    source_id aggregate_definition_source{};
};

// Complete incremental build contribution retained for one source_id.
struct source_contribution_state {
    std::vector<source_contribution_record> named;
    std::vector<std::uint32_t> anonymous_types;
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
    [[nodiscard]] status remap_new_entities(
        std::uint32_t base,
        std::span<const std::uint32_t> remap) noexcept;

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
