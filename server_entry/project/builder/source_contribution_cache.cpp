#include "source_contribution_cache.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cw::server {
namespace {

constexpr std::size_t contribution_capacity_floor = 64;

std::size_t contribution_sparse_capacity(
    std::size_t required) noexcept {

    if (required == 0) {
        return 0;
    }

    const auto extra =
        (std::max)(
            required / 8,
            contribution_capacity_floor);

    const auto maximum =
        (std::numeric_limits<std::size_t>::max)();

    return required > maximum - extra
        ? required
        : required + extra;
}

template <typename T>
void grow_contribution_vector(
    std::vector<T>& values,
    std::size_t required) {

    if (required <= values.size()) {
        return;
    }

    if (required > values.capacity()) {
        values.reserve(
            contribution_sparse_capacity(required));
    }

    values.resize(required);
}

} // namespace

status source_contribution_cache::initialize() noexcept {
    try {
        states.clear();
        candidates.clear();
        entity_states.clear();
        candidate_entities.clear();
        next_candidate_generation = 1;
        provenance_complete = true;
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

source_contribution_cache_update source_contribution_cache::begin_update(
    bool full_reconstruction) noexcept {

    auto generation = next_candidate_generation++;
    if (!generation) {
        generation = next_candidate_generation++;
    }

    return source_contribution_cache_update{*this, generation, full_reconstruction};
}

std::size_t source_contribution_cache::contribution_count(source_id source) const noexcept {
    return source && source.value() < states.size()
        ? states[source.value()].named.size()
        : 0;
}

void source_contribution_cache::invalidate() noexcept {
    states.clear();
    candidates.clear();
    entity_states.clear();
    candidate_entities.clear();
    provenance_complete = false;
    next_candidate_generation = 1;
}

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

source_contribution_storage_snapshot
source_contribution_cache::storage_snapshot_for_testing() const noexcept {
    return {
        states.data(),
        entity_states.data(),
        states.size(),
        entity_states.size(),
        states.capacity(),
        entity_states.capacity()
    };
}

#endif

source_contribution_cache_update::source_contribution_cache_update(
    source_contribution_cache& cache_owner,
    std::uint64_t update_generation,
    bool reconstruct_all_sources) noexcept
    : owner(&cache_owner),
      candidate_generation(update_generation),
      full_reconstruction(reconstruct_all_sources) {}

source_contribution_cache_update::source_contribution_cache_update(source_contribution_cache_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      changed(std::move(other.changed)),
      changed_entities(std::move(other.changed_entities)),
      candidate_generation(other.candidate_generation),
      full_reconstruction(other.full_reconstruction),
      prepared(other.prepared),
      committed_update(other.committed_update),
      prepared_states_size(other.prepared_states_size),
      prepared_entity_states_size(other.prepared_entity_states_size),
      prepared_owner_growth(other.prepared_owner_growth),
      failure(other.failure) {

    other.prepared_owner_growth = false;
}

bool source_contribution_cache_update::was_replaced(source_id source) const noexcept {
    if (!owner || !source || source.value() >= owner->candidates.size()) {
        return false;
    }

    return owner->candidates[source.value()].generation == candidate_generation;
}

const source_contribution_state* source_contribution_cache_update::committed(
    source_id source) const noexcept {

    if (!owner || !source || source.value() >= owner->states.size()) {
        return nullptr;
    }

    return &owner->states[source.value()];
}

status source_contribution_cache_update::replace(
    source_id source,
    source_contribution_state*& output) noexcept {

    output = nullptr;

    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed_update || !source || was_replaced(source)) {
        return failure = {status_code::invalid_state};
    }

    try {
        if (owner->candidates.size() <= source.value()) {
            grow_contribution_vector(owner->candidates, static_cast<std::size_t>(source.value()) + 1);
        }

        auto& slot = owner->candidates[source.value()];
        slot.generation = candidate_generation;
        slot.value = {};
        changed.push_back(source.value());
        output = &slot.value;
        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

source_contribution_state* source_contribution_cache_update::candidate(
    source_id source) noexcept {

    if (!owner || !source || source.value() >= owner->candidates.size()) {
        return nullptr;
    }

    auto& slot = owner->candidates[source.value()];
    return slot.generation == candidate_generation ? &slot.value : nullptr;
}

const source_contribution_state* source_contribution_cache_update::candidate(
    source_id source) const noexcept {

    if (!owner || !source || source.value() >= owner->candidates.size()) {
        return nullptr;
    }

    const auto& slot = owner->candidates[source.value()];
    return slot.generation == candidate_generation ? &slot.value : nullptr;
}

canonical_entity_construction_state& source_contribution_cache_update::touch_entity(
    stable_id entity) {

    if (!owner || !entity || prepared || committed_update) {
        throw std::logic_error("invalid Source contribution entity state");
    }

    const auto raw = static_cast<std::size_t>(entity.value());
    if (owner->candidate_entities.size() <= raw) {
        grow_contribution_vector(owner->candidate_entities, static_cast<std::size_t>(raw) + 1);
    }

    auto& slot = owner->candidate_entities[raw];
    if (slot.generation != candidate_generation) {
        slot.generation = candidate_generation;
        slot.value = !full_reconstruction && raw < owner->entity_states.size()
            ? owner->entity_states[raw]
            : canonical_entity_construction_state{};
        changed_entities.push_back(entity.value());
    }

    return slot.value;
}

const canonical_entity_construction_state* source_contribution_cache_update::candidate_entity(
    stable_id entity) const noexcept {

    if (!owner || !entity || entity.value() >= owner->candidate_entities.size()) {
        return nullptr;
    }

    const auto& slot = owner->candidate_entities[entity.value()];
    return slot.generation == candidate_generation ? &slot.value : nullptr;
}

status source_contribution_cache_update::remap_new_entities(
    std::uint32_t base,
    std::span<const std::uint32_t> remap) noexcept {

    if (!owner || prepared || committed_update) {
        return failure = {status_code::invalid_state};
    }

    try {
        std::vector<source_contribution_cache::candidate_entity_slot> previous(remap.size());

        for (std::size_t offset = 0; offset < remap.size(); ++offset) {
            const auto raw = static_cast<std::size_t>(base) + offset;
            if (raw < owner->candidate_entities.size() &&
                owner->candidate_entities[raw].generation == candidate_generation) {
                previous[offset] = std::move(owner->candidate_entities[raw]);
                owner->candidate_entities[raw] = {};
            }
        }

        for (std::size_t offset = 0; offset < remap.size(); ++offset) {
            const auto target = remap[offset];
            if (!target || previous[offset].generation != candidate_generation) {
                continue;
            }

            if (owner->candidate_entities.size() <= target) {
                grow_contribution_vector(owner->candidate_entities, static_cast<std::size_t>(target) + 1);
            }

            owner->candidate_entities[target] = std::move(previous[offset]);
            owner->candidate_entities[target].generation = candidate_generation;
        }

        for (auto& id : changed_entities) {
            if (id >= base && id < base + remap.size()) {
                id = remap[id - base];
            }
        }

        changed_entities.erase(
            std::remove(changed_entities.begin(), changed_entities.end(), std::uint32_t{0}),
            changed_entities.end());

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status source_contribution_cache_update::prepare_publish() noexcept {
    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed_update) {
        return failure = {status_code::invalid_state};
    }

    try {
        std::size_t maximum = 0;
        for (auto source : changed) {
            maximum = (std::max)(maximum, static_cast<std::size_t>(source));
        }

        prepared_states_size = owner->states.size();
        prepared_entity_states_size = owner->entity_states.size();
        prepared_owner_growth = true;

        if (!changed.empty() && owner->states.size() <= maximum) {
            grow_contribution_vector(
                owner->states,
                maximum + 1);
        }

        std::size_t maximum_entity = 0;
        for (auto entity : changed_entities) {
            maximum_entity = (std::max)(maximum_entity, static_cast<std::size_t>(entity));
        }
        if (!changed_entities.empty() &&
            owner->entity_states.size() <= maximum_entity) {
            grow_contribution_vector(
                owner->entity_states,
                maximum_entity + 1);
        }
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }

    prepared = true;
    return {};
}

void source_contribution_cache_update::publish_prepared() noexcept {
    if (!owner || !prepared || committed_update) {
        return;
    }

    if (full_reconstruction) {
        // G0 rebuild defines the complete Source contribution universe. Sources
        // not observed in this traversal must not retain stale build-cache state.
        for (auto& state : owner->states) {
            state.named.clear();
            state.anonymous_types.clear();
        }
        for (auto& state : owner->entity_states) {
            state = {};
        }
    }

    for (auto source : changed) {
        auto& candidate = owner->candidates[source];
        std::swap(owner->states[source], candidate.value);
    }

    for (auto entity : changed_entities) {
        auto& candidate = owner->candidate_entities[entity];
        std::swap(owner->entity_states[entity], candidate.value);
    }

    owner->provenance_complete = true;
    prepared_owner_growth = false;
    committed_update = true;
}

void source_contribution_cache_update::cancel() noexcept {
    if (committed_update) {
        return;
    }

    if (owner && prepared_owner_growth) {
        if (owner->states.size() > prepared_states_size) {
            owner->states.resize(prepared_states_size);
        }

        if (owner->entity_states.size() >
            prepared_entity_states_size) {
            owner->entity_states.resize(
                prepared_entity_states_size);
        }

        prepared_owner_growth = false;
    }

    prepared = true;
    failure = {status_code::invalid_state};
}

} // namespace cw::server
