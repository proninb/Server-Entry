#include "string_registry.hpp"

#include <cassert>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server {

status string_registry::initialize() noexcept {
    lookup_index.clear();
    records.clear();
    storage.clear();
    ++generation;
    return {};
}

string_registry_update string_registry::begin_update() noexcept {
    return string_registry_update{*this, generation};
}

string_id string_registry::find(std::string_view value) const noexcept {
    const auto found = lookup_index.find(value);

    return found == lookup_index.end()
               ? string_id{}
               : found->second;
}

std::optional<std::string_view> string_registry::get(
    string_id id) const noexcept {

    if (!id || id.value() > records.size()) {
        return std::nullopt;
    }

    const auto* record = records[id.value() - 1];

    return record
        ? std::optional<std::string_view>{std::string_view{record->bytes}}
        : std::nullopt;
}

// Reconstructs the value-to-ID index from canonical ID records and rejects any
// gap or duplicate value that would violate the dense registry contract.
status string_registry::rebuild_lookup_index() noexcept {
    try {
        lookup_type rebuilt;
        rebuilt.reserve(records.size());

        for (std::size_t index = 0; index != records.size(); ++index) {
            const auto value = static_cast<std::uint32_t>(index + 1);
            const auto* record = records[index];

            if (!record) {
                continue;
            }

            const auto [position, inserted] =
                rebuilt.emplace(
                    std::string_view{record->bytes},
                    string_id{value});

            (void)position;

            if (!inserted) {
                return {status_code::invalid_state};
            }
        }

        lookup_index.swap(rebuilt);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        return {status_code::initialization_failed};
    }
}

status string_registry::export_slots(
    std::vector<std::optional<std::string>>& output) const noexcept {

    try {
        output.clear();
        output.resize(records.size());

        for (std::size_t index = 0;
             index < records.size();
             ++index) {
            const auto* record = records[index];

            if (record) {
                output[index] = record->bytes;
            }
        }

        return {};
    }
    catch (...) {
        output.clear();
        return {status_code::initialization_failed};
    }
}

status string_registry::import_slots(
    std::span<const std::optional<std::string>> values) noexcept {

    string_registry candidate;

    if (!candidate.initialize().ok()) {
        return {status_code::initialization_failed};
    }

    try {
        candidate.records.resize(values.size());
        candidate.lookup_index.reserve(values.size());

        for (std::size_t index = 0;
             index < values.size();
             ++index) {
            if (!values[index]) {
                continue;
            }

            candidate.storage.push_back({*values[index]});
            const auto* record = &candidate.storage.back();
            candidate.records[index] = record;

            const auto id = string_id{
                static_cast<std::uint32_t>(index + 1)
            };

            const auto [position, inserted] =
                candidate.lookup_index.emplace(
                    std::string_view{record->bytes},
                    id);

            (void)position;

            if (!inserted) {
                return {status_code::artifact_corrupt};
            }
        }
    }
    catch (...) {
        return {status_code::initialization_failed};
    }

    storage.swap(candidate.storage);
    records.swap(candidate.records);
    lookup_index.swap(candidate.lookup_index);
    ++generation;
    return {};
}

void string_registry::swap_compiled(string_registry& other) noexcept {
    storage.swap(other.storage);
    records.swap(other.records);
    lookup_index.swap(other.lookup_index);
    std::swap(generation, other.generation);
}

string_registry_update::string_registry_update(
    string_registry& owner,
    std::uint64_t generation) noexcept
    : owner(&owner),
      base_generation(generation) {
}

string_registry_update::string_registry_update(
    string_registry_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      added_storage(std::move(other.added_storage)),
      added_records(std::move(other.added_records)),
      added_lookup(std::move(other.added_lookup)),
      rebuilt_storage(std::move(other.rebuilt_storage)),
      rebuilt_records(std::move(other.rebuilt_records)),
      rebuilt_lookup(std::move(other.rebuilt_lookup)),
      base_generation(other.base_generation),
      failure(other.failure),
      committed(other.committed),
      prepared(other.prepared),
      rebuild_compaction_prepared(other.rebuild_compaction_prepared) {
}

string_id string_registry_update::find(
    std::string_view value) const noexcept {

    if (owner == nullptr ||
        committed ||
        prepared ||
        owner->generation != base_generation) {
        return {};
    }

    const auto added = added_lookup.find(value);

    if (added != added_lookup.end()) {
        return added->second;
    }

    return owner->find(value);
}

status string_registry_update::intern(
    std::string_view value,
    string_id& result) noexcept {

    result = {};

    if (!failure.ok()) {
        return failure;
    }

    if (owner == nullptr ||
        committed ||
        prepared ||
        owner->generation != base_generation) {
        return {status_code::invalid_state};
    }

    if (const auto existing = find(value); existing) {
        result = existing;
        return {};
    }

    const auto next =
        owner->records.size() + added_records.size() + 1;

    if (next > std::numeric_limits<std::uint32_t>::max()) {
        return failure = {status_code::initialization_failed};
    }

    try {
        added_storage.push_back({std::string{value}});
        const auto* record = &added_storage.back();
        const auto id =
            string_id{static_cast<std::uint32_t>(next)};

        try {
            added_records.push_back(record);
        }
        catch (...) {
            added_storage.pop_back();
            throw;
        }

        try {
            const auto [position, inserted] =
                added_lookup.emplace(
                    std::string_view{record->bytes},
                    id);

            if (!inserted) {
                added_records.pop_back();
                added_storage.pop_back();
                result = position->second;
                return {};
            }
        }
        catch (...) {
            added_records.pop_back();
            added_storage.pop_back();
            throw;
        }

        result = id;
        return {};
    }
    catch (const std::bad_alloc&) {
        return failure = {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        return failure = {status_code::initialization_failed};
    }
}

std::optional<std::string_view> string_registry_update::get(
    string_id id) const noexcept {

    if (!id ||
        owner == nullptr ||
        committed ||
        prepared ||
        owner->generation != base_generation) {
        return std::nullopt;
    }

    if (id.value() <= owner->records.size()) {
        return owner->get(id);
    }

    const auto offset =
        id.value() - owner->records.size() - 1;

    if (offset >= added_records.size()) {
        return std::nullopt;
    }

    return std::string_view{added_records[offset]->bytes};
}

// Validation may read a prepared candidate because prepare_publish() freezes
// mutation but has not yet transferred candidate storage into the owner.
std::optional<std::string_view>
string_registry_update::get_for_validation(
    string_id id) const noexcept {

    if (!id ||
        owner == nullptr ||
        committed ||
        owner->generation != base_generation) {
        return std::nullopt;
    }

    if (id.value() <= owner->records.size()) {
        return owner->get(id);
    }

    const auto offset =
        id.value() - owner->records.size() - 1;

    if (offset >= added_records.size()) {
        return std::nullopt;
    }

    return std::string_view{added_records[offset]->bytes};
}

status string_registry_update::commit() noexcept {

    const auto result = prepare_publish();

    if (!result.ok()) {
        return result;
    }

    publish_prepared();
    return {};
}

status string_registry_update::prepare_publish() noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (owner == nullptr || committed || prepared) {
        return {status_code::invalid_state};
    }

    if (base_generation != owner->generation) {
        return failure = {status_code::invalid_state};
    }

    try {
        owner->records.reserve(
            owner->records.size() + added_records.size());

        owner->lookup_index.reserve(
            owner->lookup_index.size() + added_lookup.size());
    }
    catch (const std::bad_alloc&) {
        return failure = {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        return failure = {status_code::initialization_failed};
    }

    prepared = true;
    return {};
}

status string_registry_update::prepare_rebuild_compaction(
    std::span<const std::uint8_t> retained) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (owner == nullptr ||
        committed ||
        !prepared ||
        rebuild_compaction_prepared ||
        owner->generation != base_generation) {
        return failure = {status_code::invalid_state};
    }

    const auto candidate_size =
        owner->records.size() + added_records.size();

    if (retained.size() <= candidate_size) {
        return failure = {status_code::invalid_state};
    }

    try {
        rebuilt_storage.clear();
        rebuilt_records.clear();
        rebuilt_lookup.clear();

        rebuilt_records.resize(candidate_size);
        rebuilt_lookup.reserve(candidate_size);

        for (std::size_t offset = 0;
             offset < candidate_size;
             ++offset) {
            const auto id = string_id{
                static_cast<std::uint32_t>(offset + 1)
            };

            if (retained[id.value()] == 0) {
                continue;
            }

            const auto value = get_for_validation(id);

            if (!value) {
                return failure = {
                    status_code::configuration_failed
                };
            }

            rebuilt_storage.push_back({std::string{*value}});
            const auto* record = &rebuilt_storage.back();
            rebuilt_records[offset] = record;

            const auto [position, inserted] =
                rebuilt_lookup.emplace(
                    std::string_view{record->bytes},
                    id);

            (void)position;

            if (!inserted) {
                return failure = {
                    status_code::invalid_state
                };
            }
        }

        rebuild_compaction_prepared = true;
        return {};
    }
    catch (...) {
        return failure = {
            status_code::initialization_failed
        };
    }
}

// Publication transfers stable string storage and both indexes into the owner
// after all potentially allocating destination growth has already succeeded.
void string_registry_update::publish_prepared() noexcept {

    assert(prepared && !committed && owner != nullptr);

    if (rebuild_compaction_prepared) {
        owner->storage.swap(rebuilt_storage);
        owner->records.swap(rebuilt_records);
        owner->lookup_index.swap(rebuilt_lookup);

        added_storage.clear();
        added_records.clear();
        added_lookup.clear();
    }
    else {
        owner->storage.splice(
            owner->storage.end(),
            added_storage);

        owner->records.insert(
            owner->records.end(),
            added_records.begin(),
            added_records.end());
        owner->lookup_index.merge(added_lookup);

        added_records.clear();
        assert(added_lookup.empty());
    }

    ++owner->generation;
    committed = true;
}

void string_registry_update::cancel() noexcept {

    if (!committed) {
        prepared = true;
        failure = {status_code::invalid_state};
    }
}

} // namespace cw::server
