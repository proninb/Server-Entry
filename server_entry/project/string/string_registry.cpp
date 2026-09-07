#include "string_registry.hpp"
#include "string_hash.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server {
namespace {

constexpr std::size_t string_index_floor = 64;

std::size_t next_power_of_two(
    std::size_t value) noexcept {

    if (value <= 1) {
        return 1;
    }

    --value;

    for (std::size_t shift = 1;
         shift < sizeof(std::size_t) * 8;
         shift <<= 1) {
        value |= value >> shift;
    }

    return value + 1;
}

std::size_t index_capacity_for(
    std::size_t live) noexcept {

    if (live == 0) {
        return 0;
    }

    const auto maximum =
        (std::numeric_limits<std::size_t>::max)();

    if (live > maximum / 2) {
        return 0;
    }

    return next_power_of_two(
        (std::max)(
            string_index_floor,
            live * 2));
}

std::size_t bucket_for(
    std::uint64_t hash,
    std::size_t mask) noexcept {

    return static_cast<std::size_t>(
        hash ^ (hash >> 32)) & mask;
}

} // namespace

std::uint64_t string_registry::hash_value(
    std::string_view value) noexcept {

    return string_binding_hash(
        value);
}

status string_registry::initialize() noexcept {
    blocks.clear();
    records.clear();
    index.clear();
    live_count = 0;
    ++generation;
    return {};
}

string_registry_update
string_registry::begin_update() noexcept {

    return string_registry_update{
        *this,
        generation
    };
}

std::optional<std::string_view>
string_registry::get(
    string_id id) const noexcept {

    if (!id ||
        id.value() > records.size()) {
        return std::nullopt;
    }

    const auto& record =
        records[id.value() - 1];

    if (!record.live() ||
        record.block >= blocks.size()) {
        return std::nullopt;
    }

    const auto& bytes =
        blocks[record.block].bytes;

    const auto offset =
        static_cast<std::size_t>(
            record.offset);

    if (offset > bytes.size() ||
        record.length >
            bytes.size() - offset) {
        return std::nullopt;
    }

    return record.length == 0
        ? std::optional<std::string_view>{
              std::string_view{}}
        : std::optional<std::string_view>{
              std::string_view{
                  bytes.data() + offset,
                  record.length
              }};
}

string_id string_registry::find_value(
    std::string_view value,
    std::uint64_t hash) const noexcept {

    if (index.empty()) {
        return {};
    }

    const auto mask =
        index.size() - 1;

    auto bucket =
        bucket_for(
            hash,
            mask);

    for (std::size_t probe = 0;
         probe < index.size();
         ++probe) {
        const auto raw =
            index[bucket];

        if (raw == 0) {
            return {};
        }

        if (raw <= records.size()) {
            const auto& record =
                records[raw - 1];

            if (record.live() &&
                record.hash == hash &&
                record.length ==
                    value.size()) {
                const auto stored =
                    get(string_id{raw});

                if (stored &&
                    *stored == value) {
                    return string_id{raw};
                }
            }
        }

        bucket =
            (bucket + 1) & mask;
    }

    return {};
}

status string_registry::export_slots(
    std::vector<std::optional<std::string>>& output) const noexcept {

    try {
        output.clear();
        output.resize(
            records.size());

        for (std::size_t slot = 0;
             slot < records.size();
             ++slot) {
            const auto id =
                string_id{
                    static_cast<std::uint32_t>(
                        slot + 1)
                };

            const auto value =
                get(id);

            if (value) {
                output[slot] =
                    std::string{*value};
            }
        }

        return {};
    }
    catch (...) {
        output.clear();
        return {
            status_code::initialization_failed
        };
    }
}

status string_registry::import_slots(
    std::span<const std::optional<std::string>> values) noexcept {

    string_registry candidate;

    if (!candidate.initialize().ok()) {
        return {
            status_code::initialization_failed
        };
    }

    try {
        candidate.records.resize(
            values.size());

        std::size_t live = 0;
        std::size_t bytes_required = 0;

        for (const auto& value : values) {
            if (!value) {
                continue;
            }

            if (value->size() >
                (std::numeric_limits<std::uint32_t>::max)() ||
                value->size() >
                    (std::numeric_limits<std::size_t>::max)() -
                        bytes_required) {
                return {
                    status_code::artifact_corrupt
                };
            }

            bytes_required +=
                value->size();

            ++live;
        }

        if (live != 0) {
            candidate.blocks.emplace_back();

            candidate.blocks[0].bytes.reserve(
                bytes_required);

            const auto capacity =
                index_capacity_for(live);

            if (capacity == 0) {
                return {
                    status_code::initialization_failed
                };
            }

            candidate.index.assign(
                capacity,
                0);
        }

        for (std::size_t slot = 0;
             slot < values.size();
             ++slot) {
            if (!values[slot]) {
                continue;
            }

            const auto& value =
                *values[slot];

            const auto hash =
                hash_value(value);

            const auto raw =
                static_cast<std::uint32_t>(
                    slot + 1);

            const auto offset =
                candidate.blocks[0].bytes.size();

            candidate.blocks[0].bytes.insert(
                candidate.blocks[0].bytes.end(),
                value.begin(),
                value.end());

            auto& record =
                candidate.records[slot];

            record.hash = hash;
            record.offset = offset;
            record.block = 0;
            record.length =
                static_cast<std::uint32_t>(
                    value.size());

            const auto mask =
                candidate.index.size() - 1;

            auto bucket =
                bucket_for(
                    hash,
                    mask);

            bool inserted = false;

            for (std::size_t probe = 0;
                 probe < candidate.index.size();
                 ++probe) {
                const auto existing_raw =
                    candidate.index[bucket];

                if (existing_raw == 0) {
                    candidate.index[bucket] =
                        raw;

                    inserted = true;
                    break;
                }

                const auto& existing =
                    candidate.records[
                        existing_raw - 1];

                if (existing.hash == hash &&
                    existing.length ==
                        value.size()) {
                    const auto existing_value =
                        candidate.get(
                            string_id{
                                existing_raw
                            });

                    if (existing_value &&
                        *existing_value ==
                            value) {
                        return {
                            status_code::
                                artifact_corrupt
                        };
                    }
                }

                bucket =
                    (bucket + 1) & mask;
            }

            if (!inserted) {
                return {
                    status_code::initialization_failed
                };
            }
        }

        candidate.live_count =
            live;
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }

    blocks.swap(
        candidate.blocks);

    records.swap(
        candidate.records);

    index.swap(
        candidate.index);

    live_count =
        candidate.live_count;

    ++generation;
    return {};
}

void string_registry::swap_compiled(
    string_registry& other) noexcept {

    blocks.swap(
        other.blocks);

    records.swap(
        other.records);

    index.swap(
        other.index);

    std::swap(
        live_count,
        other.live_count);

    std::swap(
        generation,
        other.generation);
}

string_registry_update::string_registry_update(
    string_registry& owner,
    std::uint64_t generation) noexcept
    : owner(&owner),
      base_generation(generation) {
}

string_registry_update::string_registry_update(
    string_registry_update&& other) noexcept
    : owner(
          std::exchange(
              other.owner,
              nullptr)),
      added_records(
          std::move(
              other.added_records)),
      added_bytes(
          std::move(
              other.added_bytes)),
      added_index(
          std::move(
              other.added_index)),
      rebuilt_blocks(
          std::move(
              other.rebuilt_blocks)),
      rebuilt_records(
          std::move(
              other.rebuilt_records)),
      rebuilt_index(
          std::move(
              other.rebuilt_index)),
      rebuilt_live_count(
          other.rebuilt_live_count),
      prepared_index(
          std::move(
              other.prepared_index)),
      index_rebuild_prepared(
          other.index_rebuild_prepared),
      base_generation(
          other.base_generation),
      failure(other.failure),
      committed(other.committed),
      prepared(other.prepared),
      rebuild_compaction_prepared(
          other.rebuild_compaction_prepared) {
}

status string_registry_update::ensure_added_index(
    std::size_t required) noexcept {

    if (required == 0) {
        return {};
    }

    if (!added_index.empty() &&
        required * 2 <=
            added_index.size()) {
        return {};
    }

    const auto capacity =
        index_capacity_for(
            required);

    if (capacity == 0) {
        return {
            status_code::initialization_failed
        };
    }

    try {
        std::vector<std::uint32_t>
            rebuilt(
                capacity,
                0);

        const auto mask =
            rebuilt.size() - 1;

        for (std::size_t offset = 0;
             offset < added_records.size();
             ++offset) {
            const auto raw =
                static_cast<std::uint32_t>(
                    owner->records.size() +
                    offset +
                    1);

            auto bucket =
                bucket_for(
                    added_records[offset].hash,
                    mask);

            bool inserted = false;

            for (std::size_t probe = 0;
                 probe < rebuilt.size();
                 ++probe) {
                if (rebuilt[bucket] == 0) {
                    rebuilt[bucket] = raw;
                    inserted = true;
                    break;
                }

                bucket =
                    (bucket + 1) & mask;
            }

            if (!inserted) {
                return {
                    status_code::invalid_state
                };
            }
        }

        added_index.swap(
            rebuilt);

        return {};
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

status string_registry_update::reserve_bindings(
    std::size_t count,
    std::size_t bytes) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (owner == nullptr ||
        committed ||
        prepared ||
        owner->generation !=
            base_generation) {
        return {
            status_code::invalid_state
        };
    }

    try {
        if (count >
                (std::numeric_limits<std::size_t>::max)() -
                    added_records.size() ||
            bytes >
                (std::numeric_limits<std::size_t>::max)() -
                    added_bytes.size()) {
            return failure = {
                status_code::initialization_failed
            };
        }

        added_records.reserve(
            added_records.size() +
            count);

        added_bytes.reserve(
            added_bytes.size() +
            bytes);

        const auto result =
            ensure_added_index(
                added_records.size() +
                count);

        if (!result.ok()) {
            return failure = result;
        }

        return {};
    }
    catch (...) {
        return failure = {
            status_code::initialization_failed
        };
    }
}

status string_registry_update::bind(
    std::string_view value,
    string_id& result) noexcept {

    return bind_prehashed_one(
        value,
        string_binding_hash(value),
        result,
        false);
}

status string_registry_update::bind_prehashed(
    std::span<const prehashed_string_binding> values,
    std::span<string_id> results) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (owner == nullptr ||
        committed ||
        prepared ||
        owner->generation !=
            base_generation) {
        return {
            status_code::invalid_state
        };
    }

    if (values.size() !=
        results.size()) {
        return {
            status_code::configuration_failed
        };
    }

    std::fill(
        results.begin(),
        results.end(),
        string_id{});

    if (values.empty()) {
        return {};
    }

    if (values.size() >
        (std::numeric_limits<std::size_t>::max)() -
            added_records.size()) {
        return failure = {
            status_code::initialization_failed
        };
    }

    const auto ensure_result =
        ensure_added_index(
            added_records.size() +
            values.size());

    if (!ensure_result.ok()) {
        return failure =
            ensure_result;
    }

    for (std::size_t index = 0;
         index < values.size();
         ++index) {
        const auto& value =
            values[index];

        assert(
            value.hash ==
            string_binding_hash(
                value.value));

        const auto result =
            bind_prehashed_one(
                value.value,
                value.hash,
                results[index],
                true);

        if (!result.ok()) {
            return result;
        }
    }

    return {};
}

status string_registry_update::bind_prehashed_one(
    std::string_view value,
    std::uint64_t hash,
    string_id& result,
    bool capacity_ready) noexcept {

    result = {};

    if (!failure.ok()) {
        return failure;
    }

    if (owner == nullptr ||
        committed ||
        prepared ||
        owner->generation !=
            base_generation) {
        return {
            status_code::invalid_state
        };
    }

    if (value.size() >
        (std::numeric_limits<std::uint32_t>::max)()) {
        return failure = {
            status_code::configuration_failed
        };
    }

    // Historical identity always wins. The supplied hash only avoids repeating
    // work already performed by a Source-local Parser worker.
    if (const auto existing =
            owner->find_value(
                value,
                hash);
        existing) {
        result = existing;
        return {};
    }

    if (!capacity_ready) {
        const auto ensure_result =
            ensure_added_index(
                added_records.size() +
                1);

        if (!ensure_result.ok()) {
            return failure =
                ensure_result;
        }
    }

    if (added_index.empty()) {
        return failure = {
            status_code::invalid_state
        };
    }

    const auto mask =
        added_index.size() - 1;

    auto bucket =
        bucket_for(
            hash,
            mask);

    std::size_t empty_bucket =
        added_index.size();

    for (std::size_t probe = 0;
         probe < added_index.size();
         ++probe) {
        const auto raw =
            added_index[bucket];

        if (raw == 0) {
            empty_bucket = bucket;
            break;
        }

        if (raw >
            owner->records.size()) {
            const auto offset =
                raw -
                owner->records.size() -
                1;

            if (offset <
                added_records.size()) {
                const auto& record =
                    added_records[offset];

                if (record.hash == hash &&
                    record.length ==
                        value.size()) {
                    const auto byte_offset =
                        static_cast<std::size_t>(
                            record.offset);

                    if (byte_offset <=
                            added_bytes.size() &&
                        record.length <=
                            added_bytes.size() -
                                byte_offset) {
                        const auto stored =
                            record.length == 0
                            ? std::string_view{}
                            : std::string_view{
                                  added_bytes.data() +
                                      byte_offset,
                                  record.length
                              };

                        if (stored == value) {
                            result =
                                string_id{raw};

                            return {};
                        }
                    }
                }
            }
        }

        bucket =
            (bucket + 1) & mask;
    }

    if (empty_bucket ==
        added_index.size()) {
        return failure = {
            status_code::invalid_state
        };
    }

    const auto next =
        owner->records.size() +
        added_records.size() +
        1;

    if (next >
        (std::numeric_limits<std::uint32_t>::max)()) {
        return failure = {
            status_code::initialization_failed
        };
    }

    const auto byte_offset =
        added_bytes.size();

    try {
        added_bytes.insert(
            added_bytes.end(),
            value.begin(),
            value.end());

        string_registry::string_record
            record;

        record.hash = hash;
        record.offset = byte_offset;
        record.block =
            static_cast<std::uint32_t>(
                owner->blocks.size());
        record.length =
            static_cast<std::uint32_t>(
                value.size());

        try {
            added_records.push_back(
                record);
        }
        catch (...) {
            added_bytes.resize(
                byte_offset);

            throw;
        }

        const auto raw =
            static_cast<std::uint32_t>(
                next);

        added_index[empty_bucket] =
            raw;

        result =
            string_id{raw};

        return {};
    }
    catch (const std::bad_alloc&) {
        return failure = {
            status_code::initialization_failed
        };
    }
    catch (const std::length_error&) {
        return failure = {
            status_code::initialization_failed
        };
    }
}

std::optional<std::string_view>
string_registry_update::get(
    string_id id) const noexcept {

    if (!id ||
        owner == nullptr ||
        committed ||
        prepared ||
        owner->generation !=
            base_generation) {
        return std::nullopt;
    }

    if (id.value() <=
        owner->records.size()) {
        return owner->get(id);
    }

    const auto offset =
        id.value() -
        owner->records.size() -
        1;

    if (offset >=
        added_records.size()) {
        return std::nullopt;
    }

    const auto& record =
        added_records[offset];

    const auto byte_offset =
        static_cast<std::size_t>(
            record.offset);

    if (byte_offset >
            added_bytes.size() ||
        record.length >
            added_bytes.size() -
                byte_offset) {
        return std::nullopt;
    }

    return record.length == 0
        ? std::optional<std::string_view>{
              std::string_view{}}
        : std::optional<std::string_view>{
              std::string_view{
                  added_bytes.data() +
                      byte_offset,
                  record.length
              }};
}

std::optional<std::string_view>
string_registry_update::get_for_validation(
    string_id id) const noexcept {

    if (!id ||
        owner == nullptr ||
        committed ||
        owner->generation !=
            base_generation) {
        return std::nullopt;
    }

    if (id.value() <=
        owner->records.size()) {
        return owner->get(id);
    }

    const auto offset =
        id.value() -
        owner->records.size() -
        1;

    if (offset >=
        added_records.size()) {
        return std::nullopt;
    }

    const auto& record =
        added_records[offset];

    const auto byte_offset =
        static_cast<std::size_t>(
            record.offset);

    if (byte_offset >
            added_bytes.size() ||
        record.length >
            added_bytes.size() -
                byte_offset) {
        return std::nullopt;
    }

    return record.length == 0
        ? std::optional<std::string_view>{
              std::string_view{}}
        : std::optional<std::string_view>{
              std::string_view{
                  added_bytes.data() +
                      byte_offset,
                  record.length
              }};
}

status string_registry_update::prepare_publish() noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (owner == nullptr ||
        committed ||
        prepared ||
        owner->generation !=
            base_generation) {
        return {
            status_code::invalid_state
        };
    }

    try {
        const auto record_required =
            owner->records.size() +
            added_records.size();

        owner->records.reserve(
            record_required);

        if (!added_records.empty()) {
            if (owner->blocks.size() >=
                string_registry::invalid_block) {
                return failure = {
                    status_code::initialization_failed
                };
            }

            owner->blocks.reserve(
                owner->blocks.size() + 1);
        }

        const auto combined_live =
            owner->live_count +
            added_records.size();

        const auto required_capacity =
            index_capacity_for(
                combined_live);

        if (combined_live != 0 &&
            required_capacity == 0) {
            return failure = {
                status_code::initialization_failed
            };
        }

        if (combined_live != 0 &&
            (owner->index.empty() ||
             combined_live * 2 >
                 owner->index.size())) {
            prepared_index.assign(
                required_capacity,
                0);

            const auto mask =
                prepared_index.size() - 1;

            const auto insert_raw =
                [&](std::uint32_t raw,
                    std::uint64_t hash) noexcept {
                    auto bucket =
                        bucket_for(
                            hash,
                            mask);

                    for (std::size_t probe = 0;
                         probe <
                             prepared_index.size();
                         ++probe) {
                        if (prepared_index[
                                bucket] == 0) {
                            prepared_index[
                                bucket] = raw;

                            return true;
                        }

                        bucket =
                            (bucket + 1) & mask;
                    }

                    return false;
                };

            for (std::size_t slot = 0;
                 slot < owner->records.size();
                 ++slot) {
                const auto& record =
                    owner->records[slot];

                if (!record.live()) {
                    continue;
                }

                if (!insert_raw(
                        static_cast<std::uint32_t>(
                            slot + 1),
                        record.hash)) {
                    return failure = {
                        status_code::invalid_state
                    };
                }
            }

            for (std::size_t slot = 0;
                 slot < added_records.size();
                 ++slot) {
                if (!insert_raw(
                        static_cast<std::uint32_t>(
                            owner->records.size() +
                            slot +
                            1),
                        added_records[slot].hash)) {
                    return failure = {
                        status_code::invalid_state
                    };
                }
            }

            index_rebuild_prepared =
                true;
        }

        prepared = true;
        return {};
    }
    catch (...) {
        return failure = {
            status_code::initialization_failed
        };
    }
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
        owner->generation !=
            base_generation) {
        return failure = {
            status_code::invalid_state
        };
    }

    const auto candidate_size =
        owner->records.size() +
        added_records.size();

    if (retained.size() <=
        candidate_size) {
        return failure = {
            status_code::invalid_state
        };
    }

    bool compaction_required = false;

    for (std::size_t raw = 1;
         raw <= candidate_size;
         ++raw) {
        if (retained[raw] == 0) {
            compaction_required = true;
            break;
        }
    }

    if (!compaction_required) {
        return {};
    }

    try {
        rebuilt_blocks.clear();
        rebuilt_records.clear();
        rebuilt_index.clear();
        rebuilt_live_count = 0;

        rebuilt_records.resize(
            candidate_size);

        std::size_t bytes_required = 0;

        for (std::size_t raw = 1;
             raw <= candidate_size;
             ++raw) {
            if (retained[raw] == 0) {
                continue;
            }

            const auto value =
                get_for_validation(
                    string_id{
                        static_cast<std::uint32_t>(
                            raw)
                    });

            if (!value ||
                value->size() >
                    (std::numeric_limits<std::size_t>::max)() -
                        bytes_required) {
                return failure = {
                    status_code::configuration_failed
                };
            }

            bytes_required +=
                value->size();

            ++rebuilt_live_count;
        }

        if (rebuilt_live_count != 0) {
            rebuilt_blocks.emplace_back();

            rebuilt_blocks[0].bytes.reserve(
                bytes_required);

            const auto capacity =
                index_capacity_for(
                    rebuilt_live_count);

            if (capacity == 0) {
                return failure = {
                    status_code::initialization_failed
                };
            }

            rebuilt_index.assign(
                capacity,
                0);
        }

        for (std::size_t raw = 1;
             raw <= candidate_size;
             ++raw) {
            if (retained[raw] == 0) {
                continue;
            }

            const auto id =
                string_id{
                    static_cast<std::uint32_t>(
                        raw)
                };

            const auto value =
                get_for_validation(id);

            if (!value) {
                return failure = {
                    status_code::configuration_failed
                };
            }

            const auto hash =
                string_registry::hash_value(
                    *value);

            auto& record =
                rebuilt_records[raw - 1];

            record.hash = hash;
            record.offset =
                rebuilt_blocks[0].bytes.size();
            record.block = 0;
            record.length =
                static_cast<std::uint32_t>(
                    value->size());

            rebuilt_blocks[0].bytes.insert(
                rebuilt_blocks[0].bytes.end(),
                value->begin(),
                value->end());

            const auto mask =
                rebuilt_index.size() - 1;

            auto bucket =
                bucket_for(
                    hash,
                    mask);

            bool inserted = false;

            for (std::size_t probe = 0;
                 probe < rebuilt_index.size();
                 ++probe) {
                if (rebuilt_index[bucket] == 0) {
                    rebuilt_index[bucket] =
                        static_cast<std::uint32_t>(
                            raw);

                    inserted = true;
                    break;
                }

                bucket =
                    (bucket + 1) & mask;
            }

            if (!inserted) {
                return failure = {
                    status_code::invalid_state
                };
            }
        }

        rebuild_compaction_prepared =
            true;

        return {};
    }
    catch (...) {
        return failure = {
            status_code::initialization_failed
        };
    }
}

void string_registry_update::publish_prepared() noexcept {

    assert(
        prepared &&
        !committed &&
        owner != nullptr);

    if (rebuild_compaction_prepared) {
        owner->blocks.swap(
            rebuilt_blocks);

        owner->records.swap(
            rebuilt_records);

        owner->index.swap(
            rebuilt_index);

        owner->live_count =
            rebuilt_live_count;

        added_records.clear();
        added_bytes.clear();
        added_index.clear();
    }
    else {
        const auto first_new_raw =
            static_cast<std::uint32_t>(
                owner->records.size() +
                1);

        if (!added_records.empty()) {
            owner->blocks.emplace_back();

            owner->blocks.back().bytes.swap(
                added_bytes);

            owner->records.insert(
                owner->records.end(),
                added_records.begin(),
                added_records.end());

            owner->live_count +=
                added_records.size();

            if (index_rebuild_prepared) {
                owner->index.swap(
                    prepared_index);
            }
            else {
                const auto mask =
                    owner->index.size() - 1;

                for (std::size_t offset = 0;
                     offset < added_records.size();
                     ++offset) {
                    const auto raw =
                        first_new_raw +
                        static_cast<std::uint32_t>(
                            offset);

                    auto bucket =
                        bucket_for(
                            added_records[offset].hash,
                            mask);

                    for (;;) {
                        if (owner->index[bucket] == 0) {
                            owner->index[bucket] =
                                raw;

                            break;
                        }

                        bucket =
                            (bucket + 1) & mask;
                    }
                }
            }
        }
        else if (index_rebuild_prepared) {
            owner->index.swap(
                prepared_index);
        }

        added_records.clear();
        added_index.clear();
    }

    ++owner->generation;
    committed = true;
}

status string_registry_update::commit() noexcept {
    const auto result =
        prepare_publish();

    if (!result.ok()) {
        return result;
    }

    publish_prepared();
    return {};
}

void string_registry_update::cancel() noexcept {
    if (!committed) {
        prepared = true;
        failure = {
            status_code::invalid_state
        };
    }
}

} // namespace cw::server