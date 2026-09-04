#include "graph.hpp"

#include "compiled_state.hpp"
#include "../source/source_manager.hpp"
#include "../string/string_registry.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
#include <chrono>
#endif
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cw::server {
namespace {

constexpr std::size_t builtin_count = static_cast<std::size_t>(builtin_type::long_double_floating) + 1;
constexpr std::size_t sparse_capacity_floor = 64;

std::size_t sparse_capacity(std::size_t required) noexcept {
    if (required == 0) {
        return 0;
    }

    const auto extra = (std::max)(required / 8, sparse_capacity_floor);
    const auto maximum = (std::numeric_limits<std::size_t>::max)();

    return required > maximum - extra ? required : required + extra;
}

template <typename T>
void reserve_sparse_capacity(std::vector<T>& values, std::size_t required) {
    const auto target = sparse_capacity(required);

    if (target > values.capacity()) {
        values.reserve(target);
    }
}

template <typename T>
void grow_sparse_vector(std::vector<T>& values, std::size_t required) {
    if (required <= values.size()) {
        return;
    }

    reserve_sparse_capacity(values, required);
    values.resize(required);
}

std::uint64_t bit_mask(std::uint8_t width) noexcept {
    return width == 64 ? ~std::uint64_t{} : (std::uint64_t{1} << width) - 1;
}

bool fits_integral(integral_constant value, builtin_type target, const abi_configuration& abi) noexcept {
    const auto source_width = builtin_bit_width(value.type, abi);
    const auto target_width = builtin_bit_width(target, abi);

    if (!source_width || !target_width || !is_integral(value.type) || !is_integral(target)) {
        return false;
    }

    if (builtin_is_signed(value.type, abi)) {
        const auto signed_value = source_width == 64
            ? static_cast<std::int64_t>(value.bits)
            : static_cast<std::int64_t>(value.bits << (64 - source_width)) >> (64 - source_width);

        if (builtin_is_signed(target, abi)) {
            if (target_width == 64) {
                return true;
            }

            const auto minimum = -(std::int64_t{1} << (target_width - 1));
            const auto maximum = (std::int64_t{1} << (target_width - 1)) - 1;
            return signed_value >= minimum && signed_value <= maximum;
        }

        return signed_value >= 0 &&
            static_cast<std::uint64_t>(signed_value) <= bit_mask(target_width);
    }

    if (builtin_is_signed(target, abi)) {
        return target_width == 64
            ? value.bits <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            : value.bits < (std::uint64_t{1} << (target_width - 1));
    }

    return value.bits <= bit_mask(target_width);
}

std::uint64_t convert_integral(integral_constant value, builtin_type target,
    const abi_configuration& abi) noexcept {

    auto converted = value.bits;
    const auto source_width = builtin_bit_width(value.type, abi);

    if (builtin_is_signed(value.type, abi)) {
        const auto signed_value = source_width == 64
            ? static_cast<std::int64_t>(value.bits)
            : static_cast<std::int64_t>(value.bits << (64 - source_width)) >> (64 - source_width);

        converted = static_cast<std::uint64_t>(signed_value);
    }

    return converted & bit_mask(builtin_bit_width(target, abi));
}

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

using storage_telemetry_clock = std::chrono::steady_clock;

template <typename T, typename Operation>
void measure_vector_prepare(
    std::vector<T>& values,
    std::size_t required,
    std::size_t logical_entries_touched,
    bool execute,
    graph_vector_growth_telemetry& telemetry,
    Operation&& operation) {

    telemetry = {};
    telemetry.logical_entries_touched = logical_entries_touched;
    telemetry.required = required;
    telemetry.size_before = values.size();
    telemetry.capacity_before = values.capacity();

    const auto* data_before = values.data();

    if (execute) {
        const auto started = storage_telemetry_clock::now();
        operation();
        const auto finished = storage_telemetry_clock::now();

        telemetry.prepare_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
    }

    telemetry.size_after = values.size();
    telemetry.capacity_after = values.capacity();
    telemetry.reallocated = data_before != values.data();

    if (telemetry.reallocated) {
        telemetry.relocation_payload_bytes = telemetry.size_before * sizeof(T);
    }
}

template <typename Map, typename Operation>
void measure_hash_prepare(
    Map& values,
    std::size_t logical_entries_touched,
    bool execute,
    graph_hash_growth_telemetry& telemetry,
    Operation&& operation) {

    telemetry = {};
    telemetry.logical_entries_touched = logical_entries_touched;
    telemetry.size_before = values.size();
    telemetry.buckets_before = values.bucket_count();

    if (execute) {
        const auto started = storage_telemetry_clock::now();
        operation();
        const auto finished = storage_telemetry_clock::now();

        telemetry.prepare_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
    }

    telemetry.size_after = values.size();
    telemetry.buckets_after = values.bucket_count();
    telemetry.rehashed = telemetry.buckets_before != telemetry.buckets_after;
}

#endif

} // namespace

std::size_t graph::derived_type_key_hash::operator()(const derived_type_key& key) const noexcept {
    auto hash = static_cast<std::size_t>(key.child.value()) * 0x9e3779b1u;
    hash ^= static_cast<std::size_t>(key.kind) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    hash ^= static_cast<std::size_t>(key.payload ^ (key.payload >> 32)) +
        0x9e3779b9u + (hash << 6) + (hash >> 2);
    return hash;
}

struct graph::definition_payload {
    builtin_type underlying = builtin_type::integer;
    std::vector<enum_value_record> values;
};

struct graph::source_contribution {
    stable_id entity{};
    string_id name{};
    entity_kind kind = entity_kind::enum_type;
    enum_definition_state state = enum_definition_state::opaque;
    bool scoped = false;
    bool fixed = false;
    builtin_type underlying = builtin_type::integer;
    std::shared_ptr<const definition_payload> definition;
};

struct graph::source_state {
    std::vector<source_contribution> named;
    std::vector<std::uint32_t> anonymous_types;
};

struct graph::enum_aggregate {
    std::uint32_t declarations = 0;
    std::uint32_t scoped = 0;
    std::uint32_t unscoped = 0;
    std::uint32_t fixed = 0;
    std::uint32_t nonfixed = 0;
    std::uint32_t definitions = 0;
    std::uint32_t active_fixed = 0;

    std::uint32_t aggregate_declarations = 0;
    std::uint32_t aggregate_definitions = 0;

    std::array<std::uint32_t, builtin_count> underlying{};

    builtin_type active_type = builtin_type::integer;
    source_id definition_source{};
    std::shared_ptr<const definition_payload> definition;
    source_id aggregate_definition_source{};
};

struct graph::type_storage {
    user_type_record record;
    std::vector<enum_value_record> enumerators;
    std::vector<member_record> members;
    std::uint32_t member_begin = 0;
    std::uint32_t member_count = 0;
    bool anonymous = false;
    source_id anonymous_source{};
};

struct graph::entity_slot {
    entity_record record{};
    bool live = false;
};

struct graph::candidate_identity_slot {
    std::uint64_t generation = 0;
    stable_id value{};
};

struct graph::candidate_entity_slot {
    std::uint64_t generation = 0;
    entity_slot entity;
    enum_aggregate aggregate;
};

enum class candidate_type_kind : std::uint8_t {
    unchanged,
    replacement,
    removed
};

struct graph::candidate_type_slot {
    std::uint64_t generation = 0;
    candidate_type_kind kind = candidate_type_kind::unchanged;
    std::unique_ptr<type_storage> value;
};

struct graph::candidate_source_slot {
    std::uint64_t generation = 0;
    source_state value;
};

graph::graph() = default;
graph::~graph() = default;

status graph::initialize(abi_configuration abi) noexcept {
    try {
        types.clear();
        free_type_slots.clear();
        entities.clear();
        identity.clear();
        enum_aggregates.clear();
        source_states.clear();
        member_records.clear();

        canonical_types.clear();
        canonical_types.push_back({});

        for (std::uint32_t value = 0;
             value <= static_cast<std::uint32_t>(builtin_type::void_type);
             ++value) {
            canonical_type_record record;
            record.kind = canonical_type_kind::builtin;
            record.builtin = static_cast<builtin_type>(value);
            canonical_types.push_back(record);
        }

        named_type_refs.clear();
        derived_type_index.clear();

        candidate_identities.clear();
        candidate_entities.clear();
        candidate_types.clear();
        candidate_sources.clear();

        entity_count_value = 0;
        user_type_count_value = 0;
        next_stable_id = 1;
        next_candidate_generation = 1;
        abi_config = abi;

        ++generation;
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

graph_update graph::begin_update() noexcept {
    auto candidate = next_candidate_generation++;

    if (!candidate) {
        candidate = next_candidate_generation++;
    }

    return graph_update{*this, generation, candidate};
}

const entity_record* graph::find(string_id name) const noexcept {
    return name && name.value() < identity.size() ? find(identity[name.value()]) : nullptr;
}

const entity_record* graph::find(stable_id id) const noexcept {
    return id && id.value() < entities.size() && entities[id.value()].live
        ? &entities[id.value()].record
        : nullptr;
}

const user_type_record* graph::find(type_handle handle) const noexcept {
    return handle && handle.value() <= types.size() && types[handle.value() - 1]
        ? &types[handle.value() - 1]->record
        : nullptr;
}

std::span<const enum_value_record> graph::enum_values(type_handle handle) const noexcept {
    if (!handle || handle.value() > types.size() || !types[handle.value() - 1]) {
        return {};
    }

    return types[handle.value() - 1]->enumerators;
}

std::span<const member_record> graph::members(type_handle handle) const noexcept {
    if (!handle || handle.value() > types.size() || !types[handle.value() - 1] ||
        types[handle.value() - 1]->record.kind != user_type_kind::aggregate) {
        return {};
    }

    const auto begin = types[handle.value() - 1]->member_begin;
    const auto count = types[handle.value() - 1]->member_count;

    if (begin > member_records.size() || count > member_records.size() - begin) {
        return {};
    }

    return {member_records.data() + begin, count};
}

const member_record* graph::member(type_handle handle, member_index index) const noexcept {
    const auto values = members(handle);
    return index && index.value() <= values.size() ? &values[index.value() - 1] : nullptr;
}

member_index graph::find_member(type_handle handle, string_id name) const noexcept {
    const auto values = members(handle);

    for (std::uint32_t index = 0; index < values.size(); ++index) {
        if (values[index].name == name) {
            return member_index{index + 1};
        }
    }

    return {};
}

canonical_type_kind graph::kind(TypeRef type) const noexcept {
    return type && type.value() < canonical_types.size()
        ? canonical_types[type.value()].kind
        : canonical_type_kind::builtin;
}

bool graph::builtin(TypeRef type, builtin_type& output) const noexcept {
    if (!type || type.value() >= canonical_types.size() ||
        canonical_types[type.value()].kind != canonical_type_kind::builtin) {
        return false;
    }

    output = canonical_types[type.value()].builtin;
    return true;
}

bool graph::named(TypeRef type, type_handle& output) const noexcept {
    output = {};

    if (!type || type.value() >= canonical_types.size() ||
        canonical_types[type.value()].kind != canonical_type_kind::named) {
        return false;
    }

    output = canonical_types[type.value()].named;
    return true;
}

const derived_type_record* graph::derived(TypeRef type) const noexcept {
    return type && type.value() < canonical_types.size() &&
        canonical_types[type.value()].kind == canonical_type_kind::derived
        ? &canonical_types[type.value()].derived
        : nullptr;
}

std::size_t graph::derived_type_count() const noexcept {
    return derived_type_index.size();
}

std::size_t graph::contribution_count(source_id source) const noexcept {
    return source.value() < source_states.size() ? source_states[source.value()].named.size() : 0;
}

status graph::export_compiled(compiled_graph_state& output) const noexcept {
    try {
        compiled_graph_state candidate;
        candidate.abi = abi_config;

        candidate.identities.reserve(identity.size());
        for (auto id : identity) {
            candidate.identities.push_back(id.value());
        }

        candidate.entities.resize(entities.size());
        for (std::size_t index = 0; index < entities.size(); ++index) {
            auto& destination = candidate.entities[index];
            const auto& source = entities[index];

            destination.live = source.live;
            destination.kind = source.record.kind;
            destination.id = source.record.id.value();
            destination.name = source.record.name.value();
            destination.defining_source = source.record.defining_source.value();
            destination.type = source.record.type.value();
        }

        candidate.types.resize(types.size());
        for (std::size_t index = 0; index < types.size(); ++index) {
            if (!types[index]) {
                continue;
            }

            auto& destination = candidate.types[index];
            const auto& source = *types[index];

            destination.live = true;
            destination.record = source.record;
            destination.anonymous = source.anonymous;
            destination.anonymous_source = source.anonymous_source;
            destination.member_begin = source.member_begin;
            destination.member_count = source.member_count;

            destination.enumerators.reserve(source.enumerators.size());
            for (const auto& value : source.enumerators) {
                destination.enumerators.push_back({value.name.value(), value.bits});
            }
        }

        candidate.members.reserve(member_records.size());
        for (const auto& item : member_records) {
            candidate.members.push_back({item.name.value(), item.type.value()});
        }

        candidate.canonical_types.reserve(canonical_types.size());
        for (const auto& type : canonical_types) {
            candidate.canonical_types.push_back({
                static_cast<std::uint8_t>(type.kind),
                static_cast<std::uint8_t>(type.builtin),
                type.named.value(),
                type.derived.child.value(),
                type.derived.payload
            });
        }

        output = std::move(candidate);
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status graph::import_compiled(const compiled_graph_state& input) noexcept {
    try {
        if (!is_supported_abi_target(input.abi.target) || input.abi.pack == 0) {
            return {status_code::artifact_corrupt};
        }

        std::vector<entity_slot> imported_entities(input.entities.size());
        std::vector<enum_aggregate> imported_aggregates(input.entities.size());

        std::vector<member_record> imported_members;
        imported_members.reserve(input.members.size());

        for (const auto& item : input.members) {
            if (!item.name || !item.type_ref) {
                return {status_code::artifact_corrupt};
            }

            imported_members.push_back({string_id{item.name}, TypeRef{item.type_ref}});
        }

        std::vector<std::unique_ptr<type_storage>> imported_types(input.types.size());
        std::vector<std::uint32_t> imported_free_types;

        std::size_t imported_entity_count = 0;
        std::size_t imported_type_count = 0;

        for (std::size_t index = 0; index < input.types.size(); ++index) {
            const auto& source = input.types[index];

            if (!source.live) {
                imported_free_types.push_back(static_cast<std::uint32_t>(index + 1));
                continue;
            }

            if (source.record.kind != user_type_kind::aggregate &&
                source.record.kind != user_type_kind::enumeration) {
                return {status_code::artifact_corrupt};
            }

            if (source.record.kind == user_type_kind::aggregate && !source.enumerators.empty()) {
                return {status_code::artifact_corrupt};
            }

            if (source.record.kind == user_type_kind::enumeration &&
                source.record.enumeration.enumerator_count != source.enumerators.size()) {
                return {status_code::artifact_corrupt};
            }

            auto type = std::make_unique<type_storage>();
            type->record = source.record;
            type->anonymous = source.anonymous;
            type->anonymous_source = source.anonymous_source;
            type->member_begin = source.member_begin;
            type->member_count = source.member_count;

            type->enumerators.reserve(source.enumerators.size());
            for (const auto& item : source.enumerators) {
                type->enumerators.push_back({string_id{item.name}, item.bits});
            }

            if (type->member_begin > imported_members.size() ||
                type->member_count > imported_members.size() - type->member_begin) {
                return {status_code::artifact_corrupt};
            }

            imported_types[index] = std::move(type);
            ++imported_type_count;
        }

        for (std::size_t index = 0; index < input.entities.size(); ++index) {
            const auto& source = input.entities[index];
            auto& destination = imported_entities[index];

            destination.live = source.live;
            destination.record.id = stable_id{source.id};
            destination.record.kind = source.kind;
            destination.record.name = string_id{source.name};
            destination.record.defining_source = source_id{source.defining_source};
            destination.record.type = type_handle{source.type};

            if (!source.live) {
                continue;
            }

            if (index == 0 || source.id != index || !source.name || !source.type ||
                source.type > imported_types.size() || !imported_types[source.type - 1]) {
                return {status_code::artifact_corrupt};
            }

            const bool entity_is_aggregate = source.kind == entity_kind::aggregate_type;
            const bool type_is_aggregate =
                imported_types[source.type - 1]->record.kind == user_type_kind::aggregate;

            if (entity_is_aggregate != type_is_aggregate) {
                return {status_code::artifact_corrupt};
            }

            ++imported_entity_count;
        }

        std::vector<stable_id> imported_identities;
        imported_identities.reserve(input.identities.size());

        for (std::size_t index = 0; index < input.identities.size(); ++index) {
            const auto id = input.identities[index];

            if (id && (id >= imported_entities.size() || !imported_entities[id].live ||
                imported_entities[id].record.name.value() != index)) {
                return {status_code::artifact_corrupt};
            }

            imported_identities.push_back(stable_id{id});
        }

        types.swap(imported_types);
        free_type_slots.swap(imported_free_types);
        entities.swap(imported_entities);
        identity.swap(imported_identities);
        enum_aggregates.swap(imported_aggregates);
        source_states.clear();
        member_records.swap(imported_members);

        canonical_types.clear();
        canonical_types.push_back({});

        for (std::uint32_t value = 0;
             value <= static_cast<std::uint32_t>(builtin_type::void_type);
             ++value) {
            canonical_type_record record;
            record.kind = canonical_type_kind::builtin;
            record.builtin = static_cast<builtin_type>(value);
            canonical_types.push_back(record);
        }

        named_type_refs.assign(types.size() + 1, {});
        derived_type_index.clear();

        for (std::uint32_t handle = 1; handle <= types.size(); ++handle) {
            if (!types[handle - 1]) {
                continue;
            }

            canonical_type_record record;
            record.kind = canonical_type_kind::named;
            record.named = type_handle{handle};

            const auto raw = static_cast<std::uint32_t>(canonical_types.size());
            canonical_types.push_back(record);
            named_type_refs[handle] = TypeRef{raw};
        }

        for (std::size_t index = canonical_types.size(); index < input.canonical_types.size(); ++index) {
            const auto& source = input.canonical_types[index];

            if (source.kind != static_cast<std::uint8_t>(canonical_type_kind::derived) ||
                !source.child || source.child >= canonical_types.size()) {
                return {status_code::artifact_corrupt};
            }

            canonical_type_record record;
            record.kind = canonical_type_kind::derived;
            record.derived = {
                static_cast<derived_type_kind>(source.builtin),
                TypeRef{source.child},
                source.payload
            };

            canonical_types.push_back(record);
            derived_type_index[{record.derived.kind, record.derived.child, record.derived.payload}] =
                TypeRef{static_cast<std::uint32_t>(index)};
        }

        candidate_identities.clear();
        candidate_entities.clear();
        candidate_types.clear();
        candidate_sources.clear();

        entity_count_value = imported_entity_count;
        user_type_count_value = imported_type_count;
        next_stable_id = static_cast<std::uint32_t>(entities.size());

        if (next_stable_id == 0) {
            next_stable_id = 1;
        }

        next_candidate_generation = 1;
        abi_config = input.abi;
        ++generation;
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

void graph::swap_compiled(graph& other) noexcept {
    types.swap(other.types);
    free_type_slots.swap(other.free_type_slots);
    entities.swap(other.entities);
    identity.swap(other.identity);
    enum_aggregates.swap(other.enum_aggregates);
    source_states.swap(other.source_states);
    member_records.swap(other.member_records);
    canonical_types.swap(other.canonical_types);
    named_type_refs.swap(other.named_type_refs);
    derived_type_index.swap(other.derived_type_index);

    candidate_identities.swap(other.candidate_identities);
    candidate_entities.swap(other.candidate_entities);
    candidate_types.swap(other.candidate_types);
    candidate_sources.swap(other.candidate_sources);

    std::swap(entity_count_value, other.entity_count_value);
    std::swap(user_type_count_value, other.user_type_count_value);
    std::swap(next_stable_id, other.next_stable_id);
    std::swap(generation, other.generation);
    std::swap(abi_config, other.abi_config);
    std::swap(next_candidate_generation, other.next_candidate_generation);
}

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

namespace {

void test_hash_combine(std::uint64_t& hash, std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
}

template <typename T>
void test_hash_enum(std::uint64_t& hash, T value) noexcept {
    test_hash_combine(hash, static_cast<std::uint64_t>(value));
}

template <typename DefinitionPtr>
void test_hash_definition(
    std::uint64_t& hash,
    const DefinitionPtr& definition) noexcept {

    test_hash_combine(hash, definition ? 1 : 0);

    if (!definition) {
        return;
    }

    test_hash_enum(hash, definition->underlying);
    test_hash_combine(hash, definition->values.size());

    for (const auto& value : definition->values) {
        test_hash_combine(hash, value.name.value());
        test_hash_combine(hash, value.bits);
    }
}

} // namespace

graph_storage_snapshot graph::storage_snapshot_for_testing() const noexcept {
    graph_storage_snapshot result;

    result.identity = {identity.data(), identity.size(), identity.capacity()};
    result.entities = {entities.data(), entities.size(), entities.capacity()};
    result.enum_aggregates = {enum_aggregates.data(), enum_aggregates.size(), enum_aggregates.capacity()};
    result.source_states = {source_states.data(), source_states.size(), source_states.capacity()};
    result.types = {types.data(), types.size(), types.capacity()};
    result.free_type_slots = {free_type_slots.data(), free_type_slots.size(), free_type_slots.capacity()};
    result.member_records = {member_records.data(), member_records.size(), member_records.capacity()};
    result.canonical_types = {canonical_types.data(), canonical_types.size(), canonical_types.capacity()};
    result.named_type_refs = {named_type_refs.data(), named_type_refs.size(), named_type_refs.capacity()};
    result.derived_type_index = {derived_type_index.size(), derived_type_index.bucket_count()};

    result.entity_count = entity_count_value;
    result.user_type_count = user_type_count_value;
    result.next_stable_id = next_stable_id;
    result.generation = generation;

    std::uint64_t hash = 0xcbf29ce484222325ull;

    test_hash_combine(hash, identity.size());
    for (const auto id : identity) {
        test_hash_combine(hash, id.value());
    }

    test_hash_combine(hash, entities.size());
    for (const auto& slot : entities) {
        test_hash_combine(hash, slot.live ? 1 : 0);
        test_hash_combine(hash, slot.record.id.value());
        test_hash_enum(hash, slot.record.kind);
        test_hash_combine(hash, slot.record.name.value());
        test_hash_combine(hash, slot.record.defining_source.value());
        test_hash_combine(hash, slot.record.type.value());
    }

    test_hash_combine(hash, enum_aggregates.size());
    for (const auto& aggregate : enum_aggregates) {
        test_hash_combine(hash, aggregate.declarations);
        test_hash_combine(hash, aggregate.scoped);
        test_hash_combine(hash, aggregate.unscoped);
        test_hash_combine(hash, aggregate.fixed);
        test_hash_combine(hash, aggregate.nonfixed);
        test_hash_combine(hash, aggregate.definitions);
        test_hash_combine(hash, aggregate.active_fixed);
        test_hash_combine(hash, aggregate.aggregate_declarations);
        test_hash_combine(hash, aggregate.aggregate_definitions);

        for (const auto count : aggregate.underlying) {
            test_hash_combine(hash, count);
        }

        test_hash_enum(hash, aggregate.active_type);
        test_hash_combine(hash, aggregate.definition_source.value());
        test_hash_definition(hash, aggregate.definition);
        test_hash_combine(hash, aggregate.aggregate_definition_source.value());
    }

    test_hash_combine(hash, source_states.size());
    for (const auto& state : source_states) {
        test_hash_combine(hash, state.named.size());

        for (const auto& contribution : state.named) {
            test_hash_combine(hash, contribution.entity.value());
            test_hash_combine(hash, contribution.name.value());
            test_hash_enum(hash, contribution.kind);
            test_hash_enum(hash, contribution.state);
            test_hash_combine(hash, contribution.scoped ? 1 : 0);
            test_hash_combine(hash, contribution.fixed ? 1 : 0);
            test_hash_enum(hash, contribution.underlying);
            test_hash_definition(hash, contribution.definition);
        }

        test_hash_combine(hash, state.anonymous_types.size());
        for (const auto handle : state.anonymous_types) {
            test_hash_combine(hash, handle);
        }
    }

    test_hash_combine(hash, types.size());
    for (const auto& type : types) {
        test_hash_combine(hash, type ? 1 : 0);

        if (!type) {
            continue;
        }

        test_hash_enum(hash, type->record.kind);

        if (type->record.kind == user_type_kind::enumeration) {
            test_hash_enum(hash, type->record.enumeration.definition_state);
            test_hash_combine(hash, type->record.enumeration.scoped ? 1 : 0);
            test_hash_combine(hash, type->record.enumeration.fixed_underlying ? 1 : 0);
            test_hash_enum(hash, type->record.enumeration.underlying);
            test_hash_combine(hash, type->record.enumeration.enumerator_count);
        }
        else {
            test_hash_enum(hash, type->record.aggregate.definition_state);
        }

        test_hash_combine(hash, type->enumerators.size());
        for (const auto& value : type->enumerators) {
            test_hash_combine(hash, value.name.value());
            test_hash_combine(hash, value.bits);
        }

        test_hash_combine(hash, type->members.size());
        for (const auto& member : type->members) {
            test_hash_combine(hash, member.name.value());
            test_hash_combine(hash, member.type.value());
        }

        test_hash_combine(hash, type->member_begin);
        test_hash_combine(hash, type->member_count);
        test_hash_combine(hash, type->anonymous ? 1 : 0);
        test_hash_combine(hash, type->anonymous_source.value());
    }

    test_hash_combine(hash, free_type_slots.size());
    for (const auto handle : free_type_slots) {
        test_hash_combine(hash, handle);
    }

    test_hash_combine(hash, member_records.size());
    for (const auto& member : member_records) {
        test_hash_combine(hash, member.name.value());
        test_hash_combine(hash, member.type.value());
    }

    test_hash_combine(hash, canonical_types.size());
    for (const auto& type : canonical_types) {
        test_hash_enum(hash, type.kind);
        test_hash_enum(hash, type.builtin);
        test_hash_combine(hash, type.named.value());
        test_hash_enum(hash, type.derived.kind);
        test_hash_combine(hash, type.derived.child.value());
        test_hash_combine(hash, type.derived.payload);
    }

    test_hash_combine(hash, named_type_refs.size());
    for (const auto type : named_type_refs) {
        test_hash_combine(hash, type.value());
    }

    // derived_type_index is reconstructable from canonical_types. Its bucket
    // count is intentionally excluded because reserve() capacity is non-semantic.
    test_hash_combine(hash, derived_type_index.size());

    test_hash_combine(hash, entity_count_value);
    test_hash_combine(hash, user_type_count_value);
    test_hash_combine(hash, next_stable_id);
    test_hash_combine(hash, generation);
    test_hash_enum(hash, abi_config.target);
    test_hash_combine(hash, abi_config.pack);

    result.semantic_hash = hash;
    return result;
}

#endif

graph_update::graph_update(
    graph& owner,
    std::uint64_t generation,
    std::uint64_t candidate_generation) noexcept
    : owner(&owner),
      next_stable_id(owner.next_stable_id),
      next_type_slot(static_cast<std::uint32_t>(owner.types.size() + 1)),
      base_generation(generation),
      candidate_generation(candidate_generation) {}

graph_update::~graph_update() = default;

graph_update::graph_update(graph_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      owned_types(std::move(other.owned_types)),
      changed_identities(std::move(other.changed_identities)),
      changed_entities(std::move(other.changed_entities)),
      changed_types(std::move(other.changed_types)),
      changed_sources(std::move(other.changed_sources)),
      claimed_free_type_slots(std::move(other.claimed_free_type_slots)),
      added_canonical_types(std::move(other.added_canonical_types)),
      added_named_type_refs(std::move(other.added_named_type_refs)),
      added_named_type_index(std::move(other.added_named_type_index)),
      added_derived_type_index(std::move(other.added_derived_type_index)),
      next_stable_id(other.next_stable_id),
      next_type_slot(other.next_type_slot),
      base_generation(other.base_generation),
      candidate_generation(other.candidate_generation),
      failure(other.failure),
      prepared(other.prepared),
      committed(other.committed),
      prepared_identity_size(other.prepared_identity_size),
      prepared_entities_size(other.prepared_entities_size),
      prepared_enum_aggregates_size(other.prepared_enum_aggregates_size),
      prepared_source_states_size(other.prepared_source_states_size),
      prepared_types_size(other.prepared_types_size),
      prepared_named_type_refs_size(other.prepared_named_type_refs_size),
      prepared_owner_growth(std::exchange(other.prepared_owner_growth, false))
#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
      , storage_telemetry(other.storage_telemetry)
#endif
{
}

graph::candidate_identity_slot& graph_update::touch_identity(std::uint32_t name) {
    if (owner->candidate_identities.size() <= name) {
        grow_sparse_vector(owner->candidate_identities, static_cast<std::size_t>(name) + 1);
    }

    auto& slot = owner->candidate_identities[name];

    if (slot.generation != candidate_generation) {
        slot.generation = candidate_generation;
        slot.value = name < owner->identity.size() ? owner->identity[name] : stable_id{};
        changed_identities.push_back(name);
    }

    return slot;
}

graph::candidate_entity_slot& graph_update::touch_entity(std::uint32_t id) {
    if (owner->candidate_entities.size() <= id) {
        grow_sparse_vector(owner->candidate_entities, static_cast<std::size_t>(id) + 1);
    }

    auto& slot = owner->candidate_entities[id];

    if (slot.generation != candidate_generation) {
        slot.generation = candidate_generation;
        slot.entity = id < owner->entities.size() ? owner->entities[id] : graph::entity_slot{};
        slot.aggregate = id < owner->enum_aggregates.size()
            ? owner->enum_aggregates[id]
            : graph::enum_aggregate{};
        changed_entities.push_back(id);
    }

    return slot;
}

graph::candidate_type_slot& graph_update::touch_type(std::uint32_t handle) {
    if (owner->candidate_types.size() <= handle) {
        grow_sparse_vector(owner->candidate_types, static_cast<std::size_t>(handle) + 1);
    }

    auto& slot = owner->candidate_types[handle];

    if (slot.generation != candidate_generation) {
        slot.generation = candidate_generation;
        slot.kind = candidate_type_kind::unchanged;
        slot.value.reset();
        changed_types.push_back(handle);
    }

    return slot;
}

graph::candidate_source_slot& graph_update::touch_source(std::uint32_t source) {
    if (owner->candidate_sources.size() <= source) {
        grow_sparse_vector(owner->candidate_sources, static_cast<std::size_t>(source) + 1);
    }

    auto& slot = owner->candidate_sources[source];

    if (slot.generation != candidate_generation) {
        slot.generation = candidate_generation;
        slot.value = {};
        changed_sources.push_back(source);
    }

    return slot;
}

status graph_update::replace_source(source_id source, source_replacement& replacement) noexcept {
    replacement.update = nullptr;
    replacement.source = {};

    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed || !source) {
        return failure = {status_code::invalid_state};
    }

    if (source.value() < owner->candidate_sources.size() &&
        owner->candidate_sources[source.value()].generation == candidate_generation) {
        return failure = {status_code::duplicate_source_replacement};
    }

    const auto result = begin_source_replacement(source);

    if (!result.ok()) {
        return result;
    }

    replacement.update = this;
    replacement.source = source;
    return {};
}

status graph_update::source_replacement::add_named_enum(
    string_id name,
    const enum_build_data& data,
    stable_id& entity,
    type_handle& type) noexcept {

    return update
        ? update->declare_named_enum(name, source, data, entity, type)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::add_anonymous_enum(
    const enum_build_data& data,
    type_handle& type) noexcept {

    return update
        ? update->add_anonymous_enum(source, data, type)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::add_named_type(
    string_id name,
    aggregate_definition_state state,
    stable_id& entity,
    type_handle& type) noexcept {

    return update
        ? update->declare_named_type(name, source, state, entity, type)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::define_members(
    type_handle type,
    std::span<const member_build> input) noexcept {

    if (!update || !type) {
        return {status_code::invalid_state};
    }

    try {
        auto& slot = update->touch_type(type.value());

        if (slot.kind != candidate_type_kind::replacement || !slot.value ||
            slot.value->record.kind != user_type_kind::aggregate ||
            slot.value->record.aggregate.definition_state != aggregate_definition_state::defined) {
            return update->failure = {status_code::configuration_failed};
        }

        std::unordered_set<std::uint32_t> names;
        std::vector<member_record> members;
        members.reserve(input.size());

        for (const auto& member : input) {
            if (!member.name || !member.type.valid() ||
                !names.insert(member.name.value()).second) {
                return update->failure = {status_code::configuration_failed};
            }

            members.push_back({member.name, member.type});
        }

        slot.value->members = std::move(members);
        return {};
    }
    catch (...) {
        return update->failure = {status_code::initialization_failed};
    }
}

TypeRef graph_update::source_replacement::builtin_type_ref(builtin_type value) const noexcept {
    const auto raw = static_cast<std::uint32_t>(value) + 1;
    return update && raw < update->owner->canonical_types.size() ? TypeRef{raw} : TypeRef{};
}

status graph_update::source_replacement::resolve_type(string_id name, TypeRef& result) const noexcept {
    result = {};

    if (!update || !name) {
        return {status_code::invalid_state};
    }

    const auto* entity = update->find(name);

    if (!entity) {
        return {status_code::configuration_failed};
    }

    return update->get_or_create_named_type_ref(entity->type, result);
}

status graph_update::source_replacement::get_or_create_pointer(TypeRef child, TypeRef& output) noexcept {
    return update
        ? update->get_or_create_derived(derived_type_kind::pointer, child, 0, output)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::get_or_create_array(
    TypeRef child,
    std::uint64_t extent,
    TypeRef& output) noexcept {

    return update
        ? update->get_or_create_derived(derived_type_kind::array, child, extent, output)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::get_or_create_lvalue_reference(
    TypeRef child,
    TypeRef& output) noexcept {

    return update
        ? update->get_or_create_derived(derived_type_kind::lvalue_reference, child, 0, output)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::get_or_create_rvalue_reference(
    TypeRef child,
    TypeRef& output) noexcept {

    return update
        ? update->get_or_create_derived(derived_type_kind::rvalue_reference, child, 0, output)
        : status{status_code::invalid_state};
}

status graph_update::get_or_create_named_type_ref(type_handle handle, TypeRef& output) noexcept {
    output = {};

    if (!handle) {
        return {status_code::configuration_failed};
    }

    if (handle.value() < owner->named_type_refs.size() && owner->named_type_refs[handle.value()]) {
        output = owner->named_type_refs[handle.value()];
        return {};
    }

    if (const auto existing = added_named_type_index.find(handle.value());
        existing != added_named_type_index.end()) {
        output = existing->second;
        return {};
    }

    try {
        const auto raw = owner->canonical_types.size() + added_canonical_types.size();

        if (raw > (std::numeric_limits<std::uint32_t>::max)()) {
            return failure = {status_code::initialization_failed};
        }

        output = TypeRef{static_cast<std::uint32_t>(raw)};

        graph::canonical_type_record record;
        record.kind = canonical_type_kind::named;
        record.named = handle;

        added_canonical_types.push_back(record);
        added_named_type_refs.push_back({handle.value(), output});
        added_named_type_index.emplace(handle.value(), output);
        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::get_or_create_derived(
    derived_type_kind kind,
    TypeRef child,
    std::uint64_t payload,
    TypeRef& output) noexcept {

    output = {};

    if (!child || (kind != derived_type_kind::array && payload != 0) ||
        (kind == derived_type_kind::array && payload == 0)) {
        return failure = {status_code::configuration_failed};
    }

    const graph::derived_type_key key{kind, child, payload};

    if (const auto existing = added_derived_type_index.find(key);
        existing != added_derived_type_index.end()) {
        output = existing->second;
        return {};
    }

    if (const auto existing = owner->derived_type_index.find(key);
        existing != owner->derived_type_index.end()) {
        output = existing->second;
        return {};
    }

    try {
        const auto raw = owner->canonical_types.size() + added_canonical_types.size();

        if (raw > (std::numeric_limits<std::uint32_t>::max)()) {
            return failure = {status_code::initialization_failed};
        }

        output = TypeRef{static_cast<std::uint32_t>(raw)};

        graph::canonical_type_record record;
        record.kind = canonical_type_kind::derived;
        record.derived = {kind, child, payload};

        added_canonical_types.push_back(record);
        added_derived_type_index.emplace(key, output);
        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::build_contribution(
    const enum_build_data& data,
    graph::source_contribution& output) noexcept {

    output.kind = entity_kind::enum_type;

    if (data.definition_state == enum_definition_state::opaque && !data.enumerators.empty()) {
        return {status_code::configuration_failed};
    }

    builtin_type underlying = builtin_type::integer;
    const auto& abi = owner->abi_config;

    if (data.explicit_underlying) {
        if (!is_integral(*data.explicit_underlying)) {
            return {status_code::configuration_failed};
        }

        underlying = *data.explicit_underlying;
    }
    else if (data.scoped) {
        underlying = builtin_type::integer;
    }
    else {
        if (data.definition_state == enum_definition_state::opaque) {
            return {status_code::configuration_failed};
        }

        const auto result = select_unscoped_enum_underlying_projected(
            data.enumerators,
            abi,
            underlying,
            [](const enum_value_build& value) noexcept {
                return value.value;
            });

        if (!result.ok()) {
            return result;
        }
    }

    try {
        std::shared_ptr<graph::definition_payload> definition;

        if (data.definition_state == enum_definition_state::defined) {
            definition = std::make_shared<graph::definition_payload>();
            definition->underlying = underlying;
            definition->values.reserve(data.enumerators.size());

            for (const auto& item : data.enumerators) {
                const auto width = builtin_bit_width(item.value.type, abi);

                if (!item.name || !width || (item.value.bits & ~bit_mask(width)) ||
                    !fits_integral(item.value, underlying, abi)) {
                    return {status_code::configuration_failed};
                }

                definition->values.push_back({
                    item.name,
                    convert_integral(item.value, underlying, abi)
                });
            }
        }

        output.state = data.definition_state;
        output.scoped = data.scoped;
        output.fixed = data.explicit_underlying.has_value() || data.scoped;
        output.underlying = underlying;
        output.definition = std::move(definition);
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status graph_update::add_delta(
    source_id source,
    stable_id id,
    const graph::source_contribution& contribution) noexcept {

    try {
        auto& aggregate = touch_entity(id.value()).aggregate;

        if (contribution.kind == entity_kind::aggregate_type) {
            if (aggregate.declarations) {
                return {status_code::configuration_failed};
            }

            if (contribution.state == enum_definition_state::defined) {
                if (aggregate.aggregate_definitions) {
                    return {status_code::configuration_failed};
                }

                aggregate.aggregate_definitions = 1;
                aggregate.aggregate_definition_source = source;
            }
            else {
                ++aggregate.aggregate_declarations;
            }

            return {};
        }

        if (aggregate.aggregate_declarations || aggregate.aggregate_definitions) {
            return {status_code::configuration_failed};
        }

        if (aggregate.declarations &&
            ((contribution.scoped && aggregate.unscoped) ||
             (!contribution.scoped && aggregate.scoped) ||
             (contribution.fixed && aggregate.nonfixed) ||
             (!contribution.fixed && aggregate.fixed))) {
            return {status_code::configuration_failed};
        }

        const auto underlying_index = static_cast<std::size_t>(contribution.underlying);

        if (contribution.fixed && aggregate.active_fixed &&
            aggregate.underlying[underlying_index] == 0) {
            return {status_code::configuration_failed};
        }

        if (contribution.state == enum_definition_state::defined && aggregate.definitions) {
            return {status_code::configuration_failed};
        }

        ++aggregate.declarations;
        ++(contribution.scoped ? aggregate.scoped : aggregate.unscoped);
        ++(contribution.fixed ? aggregate.fixed : aggregate.nonfixed);

        if (contribution.fixed && aggregate.underlying[underlying_index]++ == 0) {
            ++aggregate.active_fixed;
            aggregate.active_type = contribution.underlying;
        }

        if (contribution.state == enum_definition_state::defined) {
            aggregate.definitions = 1;
            aggregate.definition_source = source;
            aggregate.definition = contribution.definition;
        }

        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status graph_update::remove_delta(
    source_id source,
    const graph::source_contribution& contribution) noexcept {

    try {
        auto& slot = touch_entity(contribution.entity.value());
        auto& aggregate = slot.aggregate;

        if (contribution.kind == entity_kind::aggregate_type) {
            if (contribution.state == enum_definition_state::defined) {
                assert(aggregate.aggregate_definition_source == source);
                aggregate.aggregate_definitions = 0;
                aggregate.aggregate_definition_source = {};
            }
            else {
                --aggregate.aggregate_declarations;
            }

            return materialize(contribution.entity, slot.entity.record.name);
        }

        --aggregate.declarations;
        --(contribution.scoped ? aggregate.scoped : aggregate.unscoped);
        --(contribution.fixed ? aggregate.fixed : aggregate.nonfixed);

        if (contribution.fixed) {
            auto& count = aggregate.underlying[static_cast<std::size_t>(contribution.underlying)];

            if (--count == 0) {
                --aggregate.active_fixed;
            }
        }

        if (contribution.state == enum_definition_state::defined) {
            assert(aggregate.definition_source == source);
            aggregate.definitions = 0;
            aggregate.definition.reset();
            aggregate.definition_source = {};
        }

        return materialize(contribution.entity, slot.entity.record.name);
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status graph_update::assign_type(
    stable_id,
    graph::entity_slot& entity,
    std::unique_ptr<graph::type_storage> type) noexcept {

    try {
        std::uint32_t handle = entity.record.type.value();

        if (!handle) {
            if (claimed_free_type_slots.size() < owner->free_type_slots.size()) {
                handle = owner->free_type_slots[
                    owner->free_type_slots.size() - 1 - claimed_free_type_slots.size()];
                claimed_free_type_slots.push_back(handle);
            }
            else {
                handle = next_type_slot++;
            }

            entity.record.type = type_handle{handle};
        }

        auto& candidate = touch_type(handle);
        candidate.kind = candidate_type_kind::replacement;
        candidate.value = std::move(type);

        TypeRef ignored;
        return get_or_create_named_type_ref(entity.record.type, ignored);
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status graph_update::materialize(stable_id id, string_id name) noexcept {
    try {
        auto& slot = touch_entity(id.value());
        auto& aggregate = slot.aggregate;

        if (!aggregate.declarations && !aggregate.aggregate_declarations &&
            !aggregate.aggregate_definitions) {
            if (slot.entity.live) {
                touch_identity(slot.entity.record.name.value()).value = {};

                if (slot.entity.record.type) {
                    auto& type = touch_type(slot.entity.record.type.value());
                    type.kind = candidate_type_kind::removed;
                    type.value.reset();
                }
            }

            slot.entity.live = false;
            return {};
        }

        auto type = std::make_unique<graph::type_storage>();

        slot.entity.live = true;
        slot.entity.record.id = id;
        slot.entity.record.name = name;
        touch_identity(name.value()).value = id;

        if (aggregate.aggregate_declarations || aggregate.aggregate_definitions) {
            const bool defined = aggregate.aggregate_definitions != 0;

            type->record.kind = user_type_kind::aggregate;
            type->record.aggregate.definition_state = defined
                ? aggregate_definition_state::defined
                : aggregate_definition_state::declared;

            if (slot.entity.record.type &&
                slot.entity.record.type.value() < owner->candidate_types.size()) {
                auto& old = owner->candidate_types[slot.entity.record.type.value()];

                if (old.generation == candidate_generation &&
                    old.kind == candidate_type_kind::replacement &&
                    old.value && old.value->record.kind == user_type_kind::aggregate) {
                    type->members = std::move(old.value->members);
                }
            }

            slot.entity.record.kind = entity_kind::aggregate_type;
            slot.entity.record.defining_source = defined
                ? aggregate.aggregate_definition_source
                : source_id{};

            return assign_type(id, slot.entity, std::move(type));
        }

        const bool defined = aggregate.definitions != 0;
        const auto underlying = defined ? aggregate.definition->underlying : aggregate.active_type;

        type->record.kind = user_type_kind::enumeration;
        type->record.enumeration.definition_state = defined
            ? enum_definition_state::defined
            : enum_definition_state::opaque;
        type->record.enumeration.scoped = aggregate.scoped != 0;
        type->record.enumeration.fixed_underlying = aggregate.fixed != 0;
        type->record.enumeration.underlying = underlying;
        type->record.enumeration.enumerator_count = defined
            ? static_cast<std::uint32_t>(aggregate.definition->values.size())
            : 0;

        if (defined) {
            type->enumerators = aggregate.definition->values;
        }

        slot.entity.record.kind = entity_kind::enum_type;
        slot.entity.record.defining_source = defined ? aggregate.definition_source : source_id{};

        return assign_type(id, slot.entity, std::move(type));
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status graph_update::begin_source_replacement(source_id source) noexcept {
    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed || !source) {
        return failure = {status_code::invalid_state};
    }

    try {
        if (source.value() < owner->candidate_sources.size() &&
            owner->candidate_sources[source.value()].generation == candidate_generation) {
            return {};
        }

        touch_source(source.value());

        if (source.value() < owner->source_states.size()) {
            for (const auto& contribution : owner->source_states[source.value()].named) {
                const auto result = remove_delta(source, contribution);

                if (!result.ok()) {
                    return failure = result;
                }
            }

            for (auto handle : owner->source_states[source.value()].anonymous_types) {
                auto& type = touch_type(handle);
                type.kind = candidate_type_kind::removed;
                type.value.reset();
            }
        }

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::declare_named_enum(
    string_id name,
    source_id source,
    const enum_build_data& data,
    stable_id& entity,
    type_handle& type) noexcept {

    entity = {};
    type = {};

    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed || !name || !source) {
        return {status_code::invalid_state};
    }

    try {
        auto result = begin_source_replacement(source);

        if (!result.ok()) {
            return failure = result;
        }

        auto& identity_slot = touch_identity(name.value());
        auto id = identity_slot.value;

        if (!id && name.value() < owner->identity.size()) {
            id = owner->identity[name.value()];
        }

        if (!id) {
            if (!next_stable_id) {
                return failure = {status_code::initialization_failed};
            }

            id = stable_id{next_stable_id++};
            identity_slot.value = id;
        }

        graph::source_contribution contribution;
        contribution.entity = id;
        contribution.name = name;

        result = build_contribution(data, contribution);

        if (!result.ok()) {
            return failure = result;
        }

        result = add_delta(source, id, contribution);

        if (!result.ok()) {
            return failure = result;
        }

        touch_source(source.value()).value.named.push_back(contribution);

        result = materialize(id, name);

        if (!result.ok()) {
            return failure = result;
        }

        entity = id;
        type = touch_entity(id.value()).entity.record.type;
        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::declare_named_type(
    string_id name,
    source_id source,
    aggregate_definition_state state,
    stable_id& entity,
    type_handle& type) noexcept {

    entity = {};
    type = {};

    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed || !name || !source) {
        return {status_code::invalid_state};
    }

    try {
        auto result = begin_source_replacement(source);

        if (!result.ok()) {
            return failure = result;
        }

        auto& identity_slot = touch_identity(name.value());
        auto id = identity_slot.value;

        if (!id && name.value() < owner->identity.size()) {
            id = owner->identity[name.value()];
        }

        if (!id) {
            if (!next_stable_id) {
                return failure = {status_code::initialization_failed};
            }

            id = stable_id{next_stable_id++};
            identity_slot.value = id;
        }

        graph::source_contribution contribution;
        contribution.entity = id;
        contribution.name = name;
        contribution.kind = entity_kind::aggregate_type;
        contribution.state = state == aggregate_definition_state::defined
            ? enum_definition_state::defined
            : enum_definition_state::opaque;

        result = add_delta(source, id, contribution);

        if (!result.ok()) {
            return failure = result;
        }

        touch_source(source.value()).value.named.push_back(contribution);

        result = materialize(id, name);

        if (!result.ok()) {
            return failure = result;
        }

        entity = id;
        type = touch_entity(id.value()).entity.record.type;
        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::add_anonymous_enum(
    source_id source,
    const enum_build_data& data,
    type_handle& type) noexcept {

    type = {};

    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed || !source) {
        return {status_code::invalid_state};
    }

    try {
        auto result = begin_source_replacement(source);

        if (!result.ok()) {
            return failure = result;
        }

        graph::source_contribution contribution;
        result = build_contribution(data, contribution);

        if (!result.ok() || data.definition_state != enum_definition_state::defined) {
            return failure = result.ok()
                ? status{status_code::configuration_failed}
                : result;
        }

        auto storage = std::make_unique<graph::type_storage>();
        storage->anonymous = true;
        storage->anonymous_source = source;
        storage->record.kind = user_type_kind::enumeration;
        storage->record.enumeration = {
            enum_definition_state::defined,
            data.scoped,
            contribution.fixed,
            contribution.underlying,
            static_cast<std::uint32_t>(contribution.definition->values.size())
        };
        storage->enumerators = contribution.definition->values;

        std::uint32_t handle = 0;

        if (claimed_free_type_slots.size() < owner->free_type_slots.size()) {
            handle = owner->free_type_slots[
                owner->free_type_slots.size() - 1 - claimed_free_type_slots.size()];
            claimed_free_type_slots.push_back(handle);
        }
        else {
            handle = next_type_slot++;
        }

        auto& candidate = touch_type(handle);
        candidate.kind = candidate_type_kind::replacement;
        candidate.value = std::move(storage);

        touch_source(source.value()).value.anonymous_types.push_back(handle);
        type = type_handle{handle};
        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

const entity_record* graph_update::find(stable_id id) const noexcept {
    if (!id || !owner || prepared || committed) {
        return nullptr;
    }

    if (id.value() < owner->candidate_entities.size()) {
        const auto& slot = owner->candidate_entities[id.value()];

        if (slot.generation == candidate_generation) {
            return slot.entity.live ? &slot.entity.record : nullptr;
        }
    }

    return owner->find(id);
}

const entity_record* graph_update::find(string_id name) const noexcept {
    if (!name || !owner || prepared || committed) {
        return nullptr;
    }

    stable_id id;

    if (name.value() < owner->candidate_identities.size() &&
        owner->candidate_identities[name.value()].generation == candidate_generation) {
        id = owner->candidate_identities[name.value()].value;
    }
    else if (name.value() < owner->identity.size()) {
        id = owner->identity[name.value()];
    }

    return find(id);
}

const user_type_record* graph_update::find(type_handle handle) const noexcept {
    if (!handle || !owner || prepared || committed) {
        return nullptr;
    }

    if (handle.value() < owner->candidate_types.size()) {
        const auto& slot = owner->candidate_types[handle.value()];

        if (slot.generation == candidate_generation) {
            if (slot.kind == candidate_type_kind::removed) {
                return nullptr;
            }

            if (slot.kind == candidate_type_kind::replacement) {
                return slot.value ? &slot.value->record : nullptr;
            }
        }
    }

    return owner->find(handle);
}

std::span<const enum_value_record> graph_update::enum_values(type_handle handle) const noexcept {
    if (!handle || !owner || prepared || committed) {
        return {};
    }

    if (handle.value() < owner->candidate_types.size()) {
        const auto& slot = owner->candidate_types[handle.value()];

        if (slot.generation == candidate_generation) {
            if (slot.kind == candidate_type_kind::removed) {
                return {};
            }

            if (slot.kind == candidate_type_kind::replacement && slot.value) {
                return slot.value->enumerators;
            }
        }
    }

    return owner->enum_values(handle);
}

status graph_update::remove_named_entity_for_testing(stable_id id) noexcept {
    if (!id || !find(id)) {
        return {status_code::invalid_state};
    }

    try {
        for (auto source : changed_sources) {
            auto& values = touch_source(source).value.named;

            for (auto position = values.begin(); position != values.end();) {
                if (position->entity != id) {
                    ++position;
                    continue;
                }

                const auto contribution = *position;
                position = values.erase(position);

                const auto result = remove_delta(source_id{source}, contribution);

                if (!result.ok()) {
                    return failure = result;
                }
            }
        }

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::prepare_publish(
    const source_manager_update& sources,
    const string_registry_update& strings) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed || owner->generation != base_generation) {
        return failure = {status_code::invalid_state};
    }

    for (auto source : changed_sources) {
        if (!sources.contains_for_validation(source_id{source})) {
            return failure = {status_code::configuration_failed};
        }

        for (const auto& contribution : owner->candidate_sources[source].value.named) {
            if (!strings.get_for_validation(contribution.name)) {
                return failure = {status_code::configuration_failed};
            }

            if (contribution.definition) {
                for (const auto& value : contribution.definition->values) {
                    if (!strings.get_for_validation(value.name)) {
                        return failure = {status_code::configuration_failed};
                    }
                }
            }
        }
    }

    std::size_t added_member_count = 0;

    for (auto handle : changed_types) {
        const auto& candidate = owner->candidate_types[handle];

        if (candidate.kind == candidate_type_kind::replacement && candidate.value) {
            added_member_count += candidate.value->members.size();

            for (const auto& member : candidate.value->members) {
                if (!strings.get_for_validation(member.name) || !member.type) {
                    return failure = {status_code::configuration_failed};
                }
            }
        }
    }

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    storage_telemetry = {};
    storage_telemetry.changed_identities = changed_identities.size();
    storage_telemetry.changed_entities = changed_entities.size();
    storage_telemetry.changed_types = changed_types.size();
    storage_telemetry.changed_sources = changed_sources.size();
    storage_telemetry.added_members = added_member_count;
    storage_telemetry.added_type_refs = added_canonical_types.size();
#endif

    prepared_identity_size = owner->identity.size();
    prepared_entities_size = owner->entities.size();
    prepared_enum_aggregates_size = owner->enum_aggregates.size();
    prepared_source_states_size = owner->source_states.size();
    prepared_types_size = owner->types.size();
    prepared_named_type_refs_size = owner->named_type_refs.size();
    prepared_owner_growth = true;

    try {
#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
        measure_vector_prepare(
            owner->identity,
            owner->candidate_identities.size(),
            changed_identities.size(),
            owner->candidate_identities.size() > owner->identity.size(),
            storage_telemetry.identity,
            [&] { grow_sparse_vector(owner->identity, owner->candidate_identities.size()); });

        measure_vector_prepare(
            owner->entities,
            owner->candidate_entities.size(),
            changed_entities.size(),
            owner->candidate_entities.size() > owner->entities.size(),
            storage_telemetry.entities,
            [&] { grow_sparse_vector(owner->entities, owner->candidate_entities.size()); });

        measure_vector_prepare(
            owner->enum_aggregates,
            owner->candidate_entities.size(),
            changed_entities.size(),
            owner->candidate_entities.size() > owner->enum_aggregates.size(),
            storage_telemetry.enum_aggregates,
            [&] { grow_sparse_vector(owner->enum_aggregates, owner->candidate_entities.size()); });

        measure_vector_prepare(
            owner->source_states,
            owner->candidate_sources.size(),
            changed_sources.size(),
            owner->candidate_sources.size() > owner->source_states.size(),
            storage_telemetry.source_states,
            [&] { grow_sparse_vector(owner->source_states, owner->candidate_sources.size()); });

        const auto needed_types = static_cast<std::size_t>(next_type_slot - 1);

        measure_vector_prepare(
            owner->types,
            needed_types,
            changed_types.size(),
            needed_types > owner->types.size(),
            storage_telemetry.types,
            [&] { grow_sparse_vector(owner->types, needed_types); });

        const auto needed_free_type_slots = owner->free_type_slots.size() + changed_types.size();
        const auto free_type_slots_target = sparse_capacity(needed_free_type_slots);

        measure_vector_prepare(
            owner->free_type_slots,
            needed_free_type_slots,
            changed_types.size(),
            free_type_slots_target > owner->free_type_slots.capacity(),
            storage_telemetry.free_type_slots,
            [&] { owner->free_type_slots.reserve(free_type_slots_target); });

        const auto needed_members = owner->member_records.size() + added_member_count;
        const auto member_target = sparse_capacity(needed_members);

        measure_vector_prepare(
            owner->member_records,
            needed_members,
            added_member_count,
            added_member_count != 0 && member_target > owner->member_records.capacity(),
            storage_telemetry.member_records,
            [&] { reserve_sparse_capacity(owner->member_records, needed_members); });

        const auto needed_canonical_types =
            owner->canonical_types.size() + added_canonical_types.size();
        const auto canonical_target = sparse_capacity(needed_canonical_types);

        measure_vector_prepare(
            owner->canonical_types,
            needed_canonical_types,
            added_canonical_types.size(),
            !added_canonical_types.empty() && canonical_target > owner->canonical_types.capacity(),
            storage_telemetry.canonical_types,
            [&] { reserve_sparse_capacity(owner->canonical_types, needed_canonical_types); });

        std::uint32_t maximum_named = 0;
        for (const auto& item : added_named_type_refs) {
            maximum_named = (std::max)(maximum_named, item.first);
        }

        const auto needed_named_type_refs = added_named_type_refs.empty()
            ? owner->named_type_refs.size()
            : static_cast<std::size_t>(maximum_named) + 1;

        measure_vector_prepare(
            owner->named_type_refs,
            needed_named_type_refs,
            added_named_type_refs.size(),
            needed_named_type_refs > owner->named_type_refs.size(),
            storage_telemetry.named_type_refs,
            [&] { grow_sparse_vector(owner->named_type_refs, needed_named_type_refs); });

        const auto needed_derived_types =
            owner->derived_type_index.size() + added_derived_type_index.size();

        measure_hash_prepare(
            owner->derived_type_index,
            added_derived_type_index.size(),
            !added_derived_type_index.empty(),
            storage_telemetry.derived_type_index,
            [&] { owner->derived_type_index.reserve(sparse_capacity(needed_derived_types)); });
#else
        grow_sparse_vector(owner->identity, owner->candidate_identities.size());
        grow_sparse_vector(owner->entities, owner->candidate_entities.size());
        grow_sparse_vector(owner->enum_aggregates, owner->candidate_entities.size());
        grow_sparse_vector(owner->source_states, owner->candidate_sources.size());

        const auto needed_types = static_cast<std::size_t>(next_type_slot - 1);
        grow_sparse_vector(owner->types, needed_types);

        const auto needed_free_type_slots = owner->free_type_slots.size() + changed_types.size();
        if (needed_free_type_slots > owner->free_type_slots.capacity()) {
            owner->free_type_slots.reserve(sparse_capacity(needed_free_type_slots));
        }

        if (added_member_count != 0) {
            reserve_sparse_capacity(owner->member_records, owner->member_records.size() + added_member_count);
        }

        if (!added_canonical_types.empty()) {
            reserve_sparse_capacity(
                owner->canonical_types,
                owner->canonical_types.size() + added_canonical_types.size());
        }

        std::uint32_t maximum_named = 0;
        for (const auto& item : added_named_type_refs) {
            maximum_named = (std::max)(maximum_named, item.first);
        }

        if (!added_named_type_refs.empty()) {
            grow_sparse_vector(owner->named_type_refs, static_cast<std::size_t>(maximum_named) + 1);
        }

        if (!added_derived_type_index.empty()) {
            owner->derived_type_index.reserve(sparse_capacity(
                owner->derived_type_index.size() + added_derived_type_index.size()));
        }
#endif
    }
    catch (...) {
        rollback_prepared_owner_growth();
        return failure = {status_code::initialization_failed};
    }

    prepared = true;
    return {};
}

void graph_update::publish_prepared() noexcept {
    assert(prepared && !committed && owner);

    for (std::size_t index = 0; index < claimed_free_type_slots.size(); ++index) {
        owner->free_type_slots.pop_back();
    }

    for (auto name : changed_identities) {
        owner->identity[name] = owner->candidate_identities[name].value;
    }

    for (auto id : changed_entities) {
        auto& candidate = owner->candidate_entities[id];
        auto& committed_entity = owner->entities[id];

        if (committed_entity.live != candidate.entity.live) {
            if (candidate.entity.live) {
                ++owner->entity_count_value;
            }
            else {
                --owner->entity_count_value;
            }
        }

        committed_entity = candidate.entity;
        owner->enum_aggregates[id] = candidate.aggregate;
    }

    for (auto handle : changed_types) {
        auto& candidate = owner->candidate_types[handle];
        const bool was_live = owner->types[handle - 1] != nullptr;
        const bool will_live = candidate.kind == candidate_type_kind::replacement ||
            (candidate.kind == candidate_type_kind::unchanged && was_live);

        if (was_live != will_live) {
            if (will_live) {
                ++owner->user_type_count_value;
            }
            else {
                --owner->user_type_count_value;
            }
        }

        if (candidate.kind == candidate_type_kind::replacement) {
            if (candidate.value->record.kind == user_type_kind::aggregate) {
                candidate.value->member_begin =
                    static_cast<std::uint32_t>(owner->member_records.size());
                candidate.value->member_count =
                    static_cast<std::uint32_t>(candidate.value->members.size());

                owner->member_records.insert(
                    owner->member_records.end(),
                    std::make_move_iterator(candidate.value->members.begin()),
                    std::make_move_iterator(candidate.value->members.end()));

                candidate.value->members.clear();
            }

            owner->types[handle - 1].swap(candidate.value);
        }
        else if (candidate.kind == candidate_type_kind::removed) {
            owner->types[handle - 1].reset();
            owner->free_type_slots.push_back(handle);
        }
    }

    for (auto source : changed_sources) {
        std::swap(owner->source_states[source], owner->candidate_sources[source].value);
    }

    owner->canonical_types.insert(
        owner->canonical_types.end(),
        added_canonical_types.begin(),
        added_canonical_types.end());

    for (const auto& item : added_named_type_refs) {
        owner->named_type_refs[item.first] = item.second;
    }

    owner->derived_type_index.merge(added_derived_type_index);
    owner->next_stable_id = next_stable_id;
    ++owner->generation;
    prepared_owner_growth = false;
    committed = true;
}

void graph_update::rollback_prepared_owner_growth() noexcept {
    if (!prepared_owner_growth || !owner || committed) {
        return;
    }

    owner->identity.resize(prepared_identity_size);
    owner->entities.resize(prepared_entities_size);
    owner->enum_aggregates.resize(prepared_enum_aggregates_size);
    owner->source_states.resize(prepared_source_states_size);
    owner->types.resize(prepared_types_size);
    owner->named_type_refs.resize(prepared_named_type_refs_size);

    prepared_owner_growth = false;
}

void graph_update::cancel() noexcept {
    if (!committed) {
        rollback_prepared_owner_growth();
        prepared = true;
        failure = {status_code::invalid_state};
    }
}

} // namespace cw::server
