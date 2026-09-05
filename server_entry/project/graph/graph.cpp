#include "graph.hpp"
#include "../builder/source_contribution_cache.hpp"

#include "compiled_state.hpp"
#include "../source/source_manager.hpp"
#include "../string/string_registry.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cw::server {
namespace {

constexpr std::size_t sparse_capacity_floor = 64;

std::size_t sparse_capacity(std::size_t required) noexcept {
    if (required == 0) {
        return 0;
    }

    const auto extra = (std::max)(required / 8, sparse_capacity_floor);
    const auto maximum = (std::numeric_limits<std::size_t>::max)();

    return required > maximum - extra
        ? required
        : required + extra;
}

template <typename T>
void reserve_sparse_capacity(
    std::vector<T>& values,
    std::size_t required) {

    const auto target = sparse_capacity(required);

    if (target > values.capacity()) {
        values.reserve(target);
    }
}

template <typename T>
void ensure_sparse_capacity(
    std::vector<T>& values,
    std::size_t required) {

    if (required > values.capacity()) {
        reserve_sparse_capacity(values, required);
    }
}

template <typename T>
void grow_sparse_vector(
    std::vector<T>& values,
    std::size_t required) {

    if (required <= values.size()) {
        return;
    }

    ensure_sparse_capacity(values, required);
    values.resize(required);
}

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

template <typename T>
graph_vector_growth_telemetry growth_before(
    const std::vector<T>& values) noexcept {

    graph_vector_growth_telemetry result;
    result.size_before = values.size();
    result.capacity_before = values.capacity();
    return result;
}

template <typename T>
void growth_after(
    const std::vector<T>& values,
    const void* data_before,
    graph_vector_growth_telemetry& result) noexcept {

    result.size_after = values.size();
    result.capacity_after = values.capacity();
    result.reallocated = data_before != values.data();

    if (result.reallocated) {
        result.relocation_payload_bytes =
            result.size_before * sizeof(T);
    }
}

#endif


std::uint64_t bit_mask(std::uint8_t width) noexcept {
    if (width == 0 || width > 64) {
        return 0;
    }

    return width == 64
        ? ~std::uint64_t{}
        : (std::uint64_t{1} << width) - 1;
}

bool fits_integral(integral_constant value, builtin_type target, const abi_configuration& abi) noexcept {

    const auto source_width = builtin_bit_width(value.type, abi);
    const auto target_width = builtin_bit_width(target, abi);

    if (!is_integral(value.type) ||
        !is_integral(target) ||
        source_width == 0 ||
        source_width > 64 ||
        target_width == 0 ||
        target_width > 64) {
        return false;
    }

    if (builtin_is_signed(value.type, abi)) {
        const auto signed_value = source_width == 64 ? static_cast<std::int64_t>(value.bits)
                : static_cast<std::int64_t>(value.bits << (64 - source_width)) >> (64 - source_width);

        if (builtin_is_signed(target, abi)) {
            if (target_width == 64) {
                return true;
            }

            const auto minimum = -(std::int64_t{1} << (target_width - 1));

            const auto maximum = (std::int64_t{1} << (target_width - 1)) - 1;

            return signed_value >= minimum && signed_value <= maximum;
        }

        return signed_value >= 0 && static_cast<std::uint64_t>(signed_value) <= bit_mask(target_width);
    }

    if (builtin_is_signed(target, abi)) {
        return target_width == 64 ? value.bits <= static_cast<std::uint64_t>(
                  (std::numeric_limits<std::int64_t>::max)()) : value.bits <
                  (std::uint64_t{1} << (target_width - 1));
    }

    return value.bits <= bit_mask(target_width);
}

std::uint64_t convert_integral(integral_constant value, builtin_type target,
    const abi_configuration& abi) noexcept {

    auto converted = value.bits;
    const auto source_width = builtin_bit_width(value.type, abi);

    if (builtin_is_signed(value.type, abi)) {
        const auto signed_value = source_width == 64 ? static_cast<std::int64_t>(value.bits)
                : static_cast<std::int64_t>(value.bits << (64 - source_width)) >> (64 - source_width);

        converted = static_cast<std::uint64_t>(signed_value);
    }

    return converted & bit_mask(builtin_bit_width(target, abi));
}

} // namespace

std::size_t graph::derived_type_key_hash::operator()(const derived_type_key& key) const noexcept {

    auto hash = static_cast<std::size_t>(key.child.value()) * 0x9e3779b1u;

    hash ^= static_cast<std::size_t>(key.kind) + 0x9e3779b9u + (hash << 6) + (hash >> 2);

    hash ^= static_cast<std::size_t>(key.payload ^ (key.payload >> 32)) + 0x9e3779b9u + (hash << 6) +
        (hash >> 2);

    return hash;
}

// Committed payload behind one type_handle. Construction-only data is kept in
// graph_update::candidate state and never survives publication into canonical G.
struct graph::type_storage {
    type_entry record;
};

// Transaction-local construction payload for one candidate type. This state is
// discarded after publication and is never persisted or exposed to Runtime.
struct graph::type_build_state {
    std::vector<enum_value_record> enumerators;
    std::vector<member_record> members;
    std::vector<member_build> pending_members;
    std::vector<type_modifier_build> pending_modifiers;
    bool definition_pending = false;
};

struct graph::entity_slot {
    entity_entry record{};
};

struct graph::candidate_identity_slot {
    std::uint64_t generation = 0;
    stable_id value{};
};

struct graph::candidate_entity_slot {
    std::uint64_t generation = 0;
    entity_slot entity;
    type_handle retained_type{};
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
    std::unique_ptr<type_build_state> build;
};

graph::graph() = default;
graph::~graph() = default;

status graph::initialize(abi_configuration abi) noexcept {
    if (!is_supported_abi_configuration(abi)) {
        return {status_code::configuration_failed};
    }

    try {
        types.clear();
        free_type_slots.clear();
        entities.clear();
        identity.clear();
        member_records.clear();
        enum_value_records.clear();

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
        type_dependencies.clear();
        reverse_type_dependents.clear();

        candidate_identities.clear();
        candidate_entities.clear();
        candidate_types.clear();

        entity_count_value = 0;
        user_type_count_value = 0;
        next_stable_id = 1;
        next_candidate_generation = 1;
        abi_config = abi;
        generation = 0;
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

graph_update graph::begin_update(
    graph_build_mode mode,
    source_contribution_cache_update& contributions) noexcept {

    auto candidate = next_candidate_generation++;

    if (!candidate) {
        candidate = next_candidate_generation++;
    }

    return graph_update{
        *this,
        contributions,
        generation,
        candidate,
        mode == graph_build_mode::rebuild
    };
}

const entity_entry* graph::find(string_id name) const noexcept {
    return name && name.value() < identity.size() ? find(identity[name.value()]) : nullptr;
}

const entity_entry* graph::find(stable_id id) const noexcept {
    return id && id.value() < entities.size() && entities[id.value()].record.live()
            ? &entities[id.value()].record
            : nullptr;
}

stable_id graph::find_id(string_id name) const noexcept {
    return name && name.value() < identity.size() ? identity[name.value()] : stable_id{};
}

const type_entry* graph::find(type_handle handle) const noexcept {
    return handle && handle.value() <= types.size() && types[handle.value() - 1]
            ? &types[handle.value() - 1]->record : nullptr;
}

std::span<const enum_value_record> graph::enum_values(type_handle handle) const noexcept {
    if (!handle || handle.value() > types.size() || !types[handle.value() - 1]) {
        return {};
    }

    const auto& type = *types[handle.value() - 1];

    if (type.record.kind != user_type_kind::enumeration || !type.record.definition) {
        return {};
    }

    const auto begin = static_cast<std::size_t>(type.record.definition.begin - 1);
    const auto count = static_cast<std::size_t>(type.record.definition.count);

    if (begin > enum_value_records.size() || count > enum_value_records.size() - begin) {
        return {};
    }

    return {enum_value_records.data() + begin, count};
}

std::span<const member_record> graph::members(type_handle handle) const noexcept {
    if (!handle || handle.value() > types.size() || !types[handle.value() - 1]) {
        return {};
    }

    const auto& type = *types[handle.value() - 1];

    if (type.record.kind != user_type_kind::aggregate || !type.record.definition) {
        return {};
    }

    const auto begin = static_cast<std::size_t>(type.record.definition.begin - 1);
    const auto count = static_cast<std::size_t>(type.record.definition.count);

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

    for (std::uint32_t index = 0;
         index < values.size();
         ++index) {
        if (values[index].name == name) {
            return member_index{index + 1};
        }
    }

    return {};
}

canonical_type_kind graph::kind(TypeRef type) const noexcept {
    return type && type.value() < canonical_types.size() ? canonical_types[type.value()].kind
            : canonical_type_kind::builtin;
}

bool graph::builtin(TypeRef type, builtin_type& output) const noexcept {

    if (!type || type.value() >= canonical_types.size() || canonical_types[type.value()].kind !=
            canonical_type_kind::builtin) {
        return false;
    }

    output = canonical_types[type.value()].builtin;

    return true;
}

bool graph::named(TypeRef type, type_handle& output) const noexcept {

    output = {};

    if (!type || type.value() >= canonical_types.size() || canonical_types[type.value()].kind !=
            canonical_type_kind::named) {
        return false;
    }

    output = canonical_types[type.value()].named;

    return true;
}

const derived_type_record* graph::derived(TypeRef type) const noexcept {

    return type && type.value() < canonical_types.size() && canonical_types[type.value()].kind ==
            canonical_type_kind::derived ? &canonical_types[type.value()].derived : nullptr;
}

std::size_t graph::derived_type_count() const noexcept {
    return derived_type_index.size();
}

status graph::rebuild_dependency_index() noexcept {
    try {
        type_dependencies.clear();
        reverse_type_dependents.clear();

        type_dependencies.resize(types.size() + 1);
        reverse_type_dependents.resize(types.size() + 1);

        const auto named_base =
            [&](TypeRef initial, type_handle& output) noexcept {
                output = {};
                auto current = initial;

                for (std::size_t depth = 0;
                     depth < canonical_types.size();
                     ++depth) {
                    if (!current ||
                        current.value() >= canonical_types.size()) {
                        return false;
                    }

                    const auto& record =
                        canonical_types[current.value()];

                    if (record.kind ==
                        canonical_type_kind::builtin) {
                        return true;
                    }

                    if (record.kind ==
                        canonical_type_kind::named) {
                        output = record.named;
                        return static_cast<bool>(output);
                    }

                    if (record.kind !=
                        canonical_type_kind::derived) {
                        return false;
                    }

                    current = record.derived.child;
                }

                return false;
            };

        for (std::uint32_t raw = 1;
             raw <= types.size();
             ++raw) {
            const auto* storage =
                types[raw - 1].get();

            if (!storage ||
                storage->record.kind !=
                    user_type_kind::aggregate ||
                !storage->record.definition) {
                continue;
            }

            const auto begin =
                static_cast<std::size_t>(
                    storage->record.definition.begin - 1);
            const auto count =
                static_cast<std::size_t>(
                    storage->record.definition.count);

            if (begin > member_records.size() ||
                count > member_records.size() - begin) {
                return {status_code::artifact_corrupt};
            }

            auto& dependencies =
                type_dependencies[raw];

            for (std::size_t index = 0;
                 index < count;
                 ++index) {
                type_handle dependency;

                if (!named_base(
                        member_records[begin + index].type,
                        dependency)) {
                    return {status_code::artifact_corrupt};
                }

                if (dependency &&
                    std::find(
                        dependencies.begin(),
                        dependencies.end(),
                        dependency.value()) ==
                        dependencies.end()) {
                    dependencies.push_back(
                        dependency.value());
                }
            }

            for (const auto dependency : dependencies) {
                if (dependency >=
                    reverse_type_dependents.size()) {
                    return {status_code::artifact_corrupt};
                }

                reverse_type_dependents[
                    dependency].push_back(raw);
            }
        }

        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
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
            const auto& source = entities[index].record;
            auto& destination = candidate.entities[index];

            destination.kind = source.kind;
            destination.name = source.name.value();
            destination.type = source.type.value();
        }

        candidate.types.resize(types.size());
        for (std::size_t index = 0; index < types.size(); ++index) {
            if (!types[index]) {
                continue;
            }

            const auto& source = *types[index];
            auto& destination = candidate.types[index];

            destination.live = true;
            destination.record = source.record;
        }

        candidate.enum_values.reserve(enum_value_records.size());
        for (const auto& value : enum_value_records) {
            candidate.enum_values.push_back({value.name.value(), value.bits});
        }

        candidate.members.reserve(member_records.size());
        for (const auto& member : member_records) {
            candidate.members.push_back({member.name.value(), member.type.value()});
        }

        candidate.canonical_types.reserve(canonical_types.size());
        for (const auto& type : canonical_types) {
            compiled_canonical_type compiled_type;
            compiled_type.kind = static_cast<std::uint8_t>(type.kind);

            switch (type.kind) {
            case canonical_type_kind::builtin:
                compiled_type.subtype = static_cast<std::uint8_t>(type.builtin);
                break;

            case canonical_type_kind::named:
                compiled_type.argument = type.named.value();
                break;

            case canonical_type_kind::derived:
                compiled_type.subtype = static_cast<std::uint8_t>(type.derived.kind);
                compiled_type.argument = type.derived.child.value();
                compiled_type.payload = type.derived.payload;
                break;
            }

            candidate.canonical_types.push_back(compiled_type);
        }

        output = std::move(candidate);
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status graph::import_compiled(
    const compiled_graph_state& input) noexcept {

    try {
        if (!is_supported_abi_configuration(input.abi)) {
            return {status_code::artifact_corrupt};
        }

        const auto builtin_entry_count =
            static_cast<std::size_t>(builtin_type::void_type) + 1;
        const auto canonical_prefix_size = builtin_entry_count + 1;

        if (input.canonical_types.size() < canonical_prefix_size ||
            input.entities.empty()) {
            return {status_code::artifact_corrupt};
        }

        std::vector<enum_value_record> imported_enum_values;
        imported_enum_values.reserve(input.enum_values.size());

        for (const auto& value : input.enum_values) {
            if (!value.name) {
                return {status_code::artifact_corrupt};
            }

            imported_enum_values.push_back({string_id{value.name}, value.bits});
        }

        std::vector<member_record> imported_members;
        imported_members.reserve(input.members.size());

        for (const auto& member : input.members) {
            if (!member.name || !member.type_ref ||
                member.type_ref >= input.canonical_types.size()) {
                return {status_code::artifact_corrupt};
            }

            imported_members.push_back({string_id{member.name}, TypeRef{member.type_ref}});
        }

        std::vector<std::unique_ptr<type_storage>> imported_types(input.types.size());
        std::vector<std::uint32_t> imported_free_types;
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

            const auto& definition = source.record.definition;
            if (!definition && definition.count != 0) {
                return {status_code::artifact_corrupt};
            }

            if (definition) {
                const auto begin = static_cast<std::size_t>(definition.begin - 1);
                const auto count = static_cast<std::size_t>(definition.count);

                if (source.record.kind == user_type_kind::aggregate) {
                    if (begin > imported_members.size() ||
                        count > imported_members.size() - begin) {
                        return {status_code::artifact_corrupt};
                    }
                }
                else {
                    if (begin > imported_enum_values.size() ||
                        count > imported_enum_values.size() - begin) {
                        return {status_code::artifact_corrupt};
                    }
                }
            }

            if (source.record.kind == user_type_kind::enumeration) {
                const auto underlying = static_cast<std::uint32_t>(source.record.enumeration.underlying);
                if (underlying > static_cast<std::uint32_t>(builtin_type::long_double_floating)) {
                    return {status_code::artifact_corrupt};
                }
            }

            auto type = std::make_unique<type_storage>();
            type->record = source.record;
            imported_types[index] = std::move(type);
            ++imported_type_count;
        }

        std::vector<entity_slot> imported_entities(input.entities.size());
        std::size_t imported_entity_count = 0;

        if (input.entities[0].live() || input.entities[0].type) {
            return {status_code::artifact_corrupt};
        }

        for (std::size_t index = 1; index < input.entities.size(); ++index) {
            const auto& source = input.entities[index];
            auto& record = imported_entities[index].record;

            if (!source.live()) {
                continue;
            }

            if (!source.name || !source.type ||
                source.type > imported_types.size() ||
                !imported_types[source.type - 1]) {
                return {status_code::artifact_corrupt};
            }

            record.kind = source.kind;
            record.name = string_id{source.name};
            record.type = type_handle{source.type};

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
        std::vector<std::uint32_t> identity_owner(input.entities.size(), 0);

        for (std::size_t name = 0; name < input.identities.size(); ++name) {
            const auto raw = input.identities[name];

            if (raw >= imported_entities.size()) {
                return {status_code::artifact_corrupt};
            }

            if (raw != 0) {
                if (identity_owner[raw] != 0 && identity_owner[raw] != name) {
                    return {status_code::artifact_corrupt};
                }

                identity_owner[raw] = static_cast<std::uint32_t>(name);

                const auto& entry = imported_entities[raw].record;
                if (entry.live() && entry.name.value() != name) {
                    return {status_code::artifact_corrupt};
                }
            }

            imported_identities.push_back(stable_id{raw});
        }

        std::vector<canonical_type_record> imported_canonical_types;
        imported_canonical_types.reserve(input.canonical_types.size());

        std::vector<TypeRef> imported_named_type_refs(imported_types.size() + 1);
        std::unordered_map<derived_type_key, TypeRef, derived_type_key_hash>
            imported_derived_type_index;
        imported_derived_type_index.reserve(input.canonical_types.size());

        for (std::size_t index = 0; index < input.canonical_types.size(); ++index) {
            const auto& source = input.canonical_types[index];
            const auto kind = static_cast<canonical_type_kind>(source.kind);
            canonical_type_record record;

            if (index == 0) {
                if (kind != canonical_type_kind::builtin ||
                    source.subtype != static_cast<std::uint8_t>(builtin_type::void_type) ||
                    source.argument != 0 || source.payload != 0) {
                    return {status_code::artifact_corrupt};
                }

                record.kind = canonical_type_kind::builtin;
                record.builtin = builtin_type::void_type;
                imported_canonical_types.push_back(record);
                continue;
            }

            if (index <= builtin_entry_count) {
                const auto expected_builtin = static_cast<builtin_type>(index - 1);

                if (kind != canonical_type_kind::builtin ||
                    source.subtype != static_cast<std::uint8_t>(expected_builtin) ||
                    source.argument != 0 || source.payload != 0) {
                    return {status_code::artifact_corrupt};
                }

                record.kind = canonical_type_kind::builtin;
                record.builtin = expected_builtin;
                imported_canonical_types.push_back(record);
                continue;
            }

            switch (kind) {
            case canonical_type_kind::builtin:
                return {status_code::artifact_corrupt};

            case canonical_type_kind::named:
                if (source.subtype != 0 || source.argument == 0 ||
                    source.argument > imported_types.size() || source.payload != 0 ||
                    imported_named_type_refs[source.argument]) {
                    return {status_code::artifact_corrupt};
                }

                record.kind = canonical_type_kind::named;
                record.named = type_handle{source.argument};
                imported_named_type_refs[source.argument] =
                    TypeRef{static_cast<std::uint32_t>(index)};
                break;

            case canonical_type_kind::derived: {
                if (source.subtype > static_cast<std::uint8_t>(derived_type_kind::rvalue_reference) ||
                    source.argument == 0 || source.argument >= index) {
                    return {status_code::artifact_corrupt};
                }

                const auto derived_kind = static_cast<derived_type_kind>(source.subtype);
                if ((derived_kind == derived_type_kind::array && source.payload == 0) ||
                    (derived_kind != derived_type_kind::array && source.payload != 0)) {
                    return {status_code::artifact_corrupt};
                }

                record.kind = canonical_type_kind::derived;
                record.derived = {derived_kind, TypeRef{source.argument}, source.payload};

                const derived_type_key key{
                    record.derived.kind,
                    record.derived.child,
                    record.derived.payload
                };

                if (!imported_derived_type_index.emplace(
                        key, TypeRef{static_cast<std::uint32_t>(index)}).second) {
                    return {status_code::artifact_corrupt};
                }

                break;
            }

            default:
                return {status_code::artifact_corrupt};
            }

            imported_canonical_types.push_back(record);
        }

        for (const auto& entity : imported_entities) {
            if (entity.record.live() &&
                !imported_named_type_refs[entity.record.type.value()]) {
                return {status_code::artifact_corrupt};
            }
        }

        std::vector<std::uint8_t> canonical_live(imported_canonical_types.size(), 0);

        for (std::size_t index = 1; index < imported_canonical_types.size(); ++index) {
            const auto& type = imported_canonical_types[index];

            switch (type.kind) {
            case canonical_type_kind::builtin:
                canonical_live[index] = 1;
                break;

            case canonical_type_kind::named:
                canonical_live[index] =
                    type.named && type.named.value() <= imported_types.size() &&
                    imported_types[type.named.value() - 1]
                        ? 1
                        : 0;
                break;

            case canonical_type_kind::derived:
                canonical_live[index] =
                    type.derived.child && type.derived.child.value() < index &&
                    canonical_live[type.derived.child.value()]
                        ? 1
                        : 0;
                break;

            default:
                return {status_code::artifact_corrupt};
            }
        }

        for (const auto& type : imported_types) {
            if (!type || type->record.kind != user_type_kind::aggregate ||
                !type->record.definition) {
                continue;
            }

            const auto begin = static_cast<std::size_t>(type->record.definition.begin - 1);
            const auto count = static_cast<std::size_t>(type->record.definition.count);

            for (std::size_t index = 0; index < count; ++index) {
                const auto reference = imported_members[begin + index].type.value();

                if (reference == 0 || reference >= canonical_live.size() ||
                    canonical_live[reference] == 0) {
                    return {status_code::artifact_corrupt};
                }
            }
        }

        types.swap(imported_types);
        free_type_slots.swap(imported_free_types);
        entities.swap(imported_entities);
        identity.swap(imported_identities);

        member_records.swap(imported_members);
        enum_value_records.swap(imported_enum_values);
        canonical_types.swap(imported_canonical_types);
        named_type_refs.swap(imported_named_type_refs);
        derived_type_index.swap(imported_derived_type_index);

        const auto dependency_result =
            rebuild_dependency_index();

        if (!dependency_result.ok()) {
            return dependency_result;
        }

        candidate_identities.clear();
        candidate_entities.clear();
        candidate_types.clear();

        entity_count_value = imported_entity_count;
        user_type_count_value = imported_type_count;
        next_stable_id = static_cast<std::uint32_t>(entities.size());
        if (next_stable_id == 0) {
            next_stable_id = 1;
        }

        next_candidate_generation = 1;
        abi_config = input.abi;
        generation = 0;
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
    member_records.swap(other.member_records);
    enum_value_records.swap(other.enum_value_records);
    canonical_types.swap(other.canonical_types);
    named_type_refs.swap(other.named_type_refs);
    derived_type_index.swap(other.derived_type_index);
    type_dependencies.swap(other.type_dependencies);
    reverse_type_dependents.swap(other.reverse_type_dependents);

    candidate_identities.swap(other.candidate_identities);
    candidate_entities.swap(other.candidate_entities);
    candidate_types.swap(other.candidate_types);

    std::swap(entity_count_value, other.entity_count_value);

    std::swap(user_type_count_value, other.user_type_count_value);

    std::swap(next_stable_id, other.next_stable_id);

    std::swap(generation, other.generation);

    std::swap(abi_config, other.abi_config);

    std::swap(next_candidate_generation, other.next_candidate_generation);
}

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

graph_storage_snapshot graph::storage_snapshot_for_testing() const noexcept {
    graph_storage_snapshot result;

    result.identity_data = identity.data();
    result.entities_data = entities.data();
    result.types_data = types.data();
    result.member_records_data = member_records.data();
    result.enum_value_records_data = enum_value_records.data();
    result.canonical_types_data = canonical_types.data();
    result.named_type_refs_data = named_type_refs.data();

    result.identity_size = identity.size();
    result.entities_size = entities.size();
    result.types_size = types.size();
    result.member_records_size = member_records.size();
    result.enum_value_records_size = enum_value_records.size();
    result.canonical_types_size = canonical_types.size();
    result.named_type_refs_size = named_type_refs.size();

    result.identity_capacity = identity.capacity();
    result.entities_capacity = entities.capacity();
    result.types_capacity = types.capacity();
    result.member_records_capacity = member_records.capacity();
    result.enum_value_records_capacity = enum_value_records.capacity();
    result.canonical_types_capacity = canonical_types.capacity();
    result.named_type_refs_capacity = named_type_refs.capacity();

    return result;
}

#endif

graph_update::graph_update(
    graph& graph_owner,
    source_contribution_cache_update& contribution_update,
    std::uint64_t generation,
    std::uint64_t update_generation,
    bool reconstruct_all_sources) noexcept
    : owner(&graph_owner),
      contributions(&contribution_update),
      next_stable_id(graph_owner.next_stable_id),
      next_type_slot(reconstruct_all_sources
          ? 1u
          : static_cast<std::uint32_t>(graph_owner.types.size() + 1)),
      base_generation(generation),
      candidate_generation(update_generation),
      full_reconstruction(reconstruct_all_sources) {}

graph_update::~graph_update() = default;

graph_update::graph_update(graph_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      contributions(std::exchange(other.contributions, nullptr)),
      owned_types(std::move(other.owned_types)),
      changed_identities(std::move(other.changed_identities)),
      changed_entities(std::move(other.changed_entities)),
      changed_types(std::move(other.changed_types)),
      changed_sources(std::move(other.changed_sources)),
      claimed_free_type_slots(std::move(other.claimed_free_type_slots)),
      rebuilt_identity(std::move(other.rebuilt_identity)),
      rebuilt_entities(std::move(other.rebuilt_entities)),
      rebuilt_types(std::move(other.rebuilt_types)),
      rebuilt_free_type_slots(std::move(other.rebuilt_free_type_slots)),
      rebuilt_entity_count(other.rebuilt_entity_count),
      rebuilt_type_count(other.rebuilt_type_count),
      rebuilt_member_records(std::move(other.rebuilt_member_records)),
      rebuilt_enum_value_records(std::move(other.rebuilt_enum_value_records)),
      rebuilt_canonical_types(std::move(other.rebuilt_canonical_types)),
      rebuilt_named_type_refs(std::move(other.rebuilt_named_type_refs)),
      rebuilt_derived_type_index(std::move(other.rebuilt_derived_type_index)),
      rebuilt_type_dependencies(std::move(other.rebuilt_type_dependencies)),
      rebuilt_reverse_type_dependents(std::move(other.rebuilt_reverse_type_dependents)),
      added_canonical_types(std::move(other.added_canonical_types)),
      added_named_type_refs(std::move(other.added_named_type_refs)),
      added_named_type_index(std::move(other.added_named_type_index)),
      added_derived_type_index(std::move(other.added_derived_type_index)),
      prepared_type_dependency_updates(
          std::move(other.prepared_type_dependency_updates)),
      prepared_reverse_dependency_updates(
          std::move(other.prepared_reverse_dependency_updates)),
      prepared_identity_size(other.prepared_identity_size),
      prepared_entities_size(other.prepared_entities_size),
      prepared_types_size(other.prepared_types_size),
      prepared_named_type_refs_size(other.prepared_named_type_refs_size),
      prepared_type_dependencies_size(other.prepared_type_dependencies_size),
      prepared_reverse_type_dependents_size(
          other.prepared_reverse_type_dependents_size),
      prepared_owner_growth(other.prepared_owner_growth),
      next_stable_id(other.next_stable_id),
      next_type_slot(other.next_type_slot),
      base_generation(other.base_generation),
      candidate_generation(other.candidate_generation),
      full_reconstruction(other.full_reconstruction),
      failure(other.failure),
      prepared(other.prepared),
      committed(other.committed)
#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
      , storage_telemetry(other.storage_telemetry)
#endif
{
    other.prepared_owner_growth = false;
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

        slot.entity = !full_reconstruction && id < owner->entities.size()
            ? owner->entities[id]
            : graph::entity_slot{};

        slot.retained_type =
            !full_reconstruction
                ? slot.entity.record.type
                : type_handle{};

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
        slot.build.reset();

        changed_types.push_back(handle);
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

    if (!contributions || contributions->was_replaced(source)) {
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

status graph_update::source_replacement::add_named_enum(string_id name, const enum_build_data& data,
    stable_id& entity, type_handle& type) noexcept {

    return update ? update->declare_named_enum(name, source, data, entity, type)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::add_anonymous_enum(const enum_build_data& data,
    type_handle& type) noexcept {

    return update ? update->add_anonymous_enum(source, data, type) : status{status_code::invalid_state};
}

status graph_update::source_replacement::add_named_type(string_id name, aggregate_definition_state state,
    stable_id& entity, type_handle& type) noexcept {

    return update ? update->declare_named_type(name, source, state, entity, type)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::define_members(
    type_handle type,
    std::span<const member_build> input,
    std::span<const type_modifier_build> modifiers) noexcept {

    if (!update || !type) {
        return {status_code::invalid_state};
    }

    try {
        auto& slot = update->touch_type(type.value());

        if (slot.kind != candidate_type_kind::replacement ||
            !slot.value ||
            slot.value->record.kind != user_type_kind::aggregate ||
            !slot.build || !slot.build->definition_pending) {
            return update->failure =
                {status_code::configuration_failed};
        }

        std::unordered_set<std::uint32_t> names;
        std::vector<member_build> copied_members;
        copied_members.reserve(input.size());

        for (const auto& member : input) {
            if (!member.name ||
                (member.builtin.has_value() ==
                 static_cast<bool>(member.user_type_name)) ||
                !names.insert(member.name.value()).second ||
                member.modifier_offset > modifiers.size() ||
                member.modifier_count >
                    modifiers.size() - member.modifier_offset) {
                return update->failure =
                    {status_code::configuration_failed};
            }

            copied_members.push_back(member);
        }

        slot.build->pending_members =
            std::move(copied_members);

        slot.build->pending_modifiers.assign(
            modifiers.begin(),
            modifiers.end());

        slot.build->members.clear();
        return {};
    }
    catch (...) {
        return update->failure =
            {status_code::initialization_failed};
    }
}

TypeRef graph_update::source_replacement::builtin_type_ref(builtin_type value) const noexcept {

    const auto raw = static_cast<std::uint32_t>(value) + 1;

    return update && raw < update->owner->canonical_types.size() ? TypeRef{raw}
            : TypeRef{};
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

    return update ? update->get_or_create_derived(derived_type_kind::pointer, child, 0, output)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::get_or_create_array(TypeRef child, std::uint64_t extent,
    TypeRef& output) noexcept {

    return update ? update->get_or_create_derived(derived_type_kind::array, child, extent, output)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::get_or_create_lvalue_reference(TypeRef child,
    TypeRef& output) noexcept {

    return update ? update->get_or_create_derived(derived_type_kind::lvalue_reference, child, 0, output)
        : status{status_code::invalid_state};
}

status graph_update::source_replacement::get_or_create_rvalue_reference(TypeRef child,
    TypeRef& output) noexcept {

    return update ? update->get_or_create_derived(derived_type_kind::rvalue_reference, child, 0, output)
        : status{status_code::invalid_state};
}

status graph_update::get_or_create_named_type_ref(type_handle handle, TypeRef& output) noexcept {

    output = {};

    if (!handle) {
        return {status_code::configuration_failed};
    }

    if (!full_reconstruction &&
        handle.value() < owner->named_type_refs.size() &&
        owner->named_type_refs[handle.value()]) {
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

        output = TypeRef{
                static_cast<std::uint32_t>(raw) };

        graph::canonical_type_record record;
        record.kind = canonical_type_kind::named;
        record.named = handle;

        added_canonical_types.push_back(record);

        added_named_type_refs.push_back({
            handle.value(), output });

        added_named_type_index.emplace(handle.value(), output);

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::get_or_create_derived(derived_type_kind kind, TypeRef child, std::uint64_t payload,
    TypeRef& output) noexcept {

    output = {};

    if (!child || (kind != derived_type_kind::array && payload != 0) || (kind == derived_type_kind::array &&
         payload == 0)) {
        return failure = {status_code::configuration_failed};
    }

    const graph::derived_type_key key{
        kind, child, payload };

    if (const auto existing = added_derived_type_index.find(key);
        existing != added_derived_type_index.end()) {
        output = existing->second;
        return {};
    }

    if (!full_reconstruction) {
        if (const auto existing = owner->derived_type_index.find(key);
            existing != owner->derived_type_index.end()) {
            output = existing->second;
            return {};
        }
    }

    try {
        const auto raw = owner->canonical_types.size() + added_canonical_types.size();

        if (raw > (std::numeric_limits<std::uint32_t>::max)()) {
            return failure = {status_code::initialization_failed};
        }

        output = TypeRef{
                static_cast<std::uint32_t>(raw) };

        graph::canonical_type_record record;
        record.kind = canonical_type_kind::derived;
        record.derived = {
            kind, child, payload };

        added_canonical_types.push_back(record);

        added_derived_type_index.emplace(key, output);

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::build_contribution(const enum_build_data& data,
    source_contribution_record& output) noexcept {

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

        const auto result = select_unscoped_enum_underlying_projected(data.enumerators, abi, underlying,
                [](const enum_value_build& value) noexcept {
                    return value.value;
                });

        if (!result.ok()) {
            return result;
        }
    }

    try {
        std::shared_ptr<source_definition_payload> definition;

        if (data.definition_state == enum_definition_state::defined) {
            definition = std::make_shared< source_definition_payload>();

            definition->underlying = underlying;

            definition->values.reserve(data.enumerators.size());

            for (const auto& item : data.enumerators) {
                const auto width = builtin_bit_width(item.value.type, abi);

                if (!item.name ||
                    !is_integral(item.value.type) ||
                    width == 0 ||
                    width > 64 ||
                    (item.value.bits & ~bit_mask(width)) ||
                    !fits_integral(item.value, underlying, abi)) {
                    return {
                        status_code::configuration_failed };
                }

                definition->values.push_back({
                    item.name, convert_integral(item.value, underlying, abi) });
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

status graph_update::add_delta(source_id source, stable_id id,
    const source_contribution_record& contribution) noexcept {

    try {
        auto& aggregate = contributions->touch_entity(id);

        if (contribution.kind == entity_kind::aggregate_type) {
            if (aggregate.declarations) {
                return {status_code::configuration_failed};
            }

            if (contribution.state == enum_definition_state::defined) {
                if (aggregate.aggregate_definitions) {
                    return {
                        status_code::configuration_failed };
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

        if (aggregate.declarations && ((contribution.scoped && aggregate.unscoped) || (!contribution.scoped &&
              aggregate.scoped) || (contribution.fixed && aggregate.nonfixed) || (!contribution.fixed &&
              aggregate.fixed))) {
            return {status_code::configuration_failed};
        }

        const auto underlying_index = static_cast<std::size_t>(contribution.underlying);

        if (contribution.fixed && aggregate.active_fixed && aggregate.underlying[underlying_index] == 0) {
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

status graph_update::remove_delta(source_id source, const source_contribution_record& contribution) noexcept {

    try {
        auto& slot = touch_entity(contribution.entity.value());
        auto& aggregate = contributions->touch_entity(contribution.entity);


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
            auto& count = aggregate.underlying[ static_cast<std::size_t>(contribution.underlying)];

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

status graph_update::assign_type(stable_id id, graph::entity_slot& entity,
    std::unique_ptr<graph::type_storage> type) noexcept {

    try {
        std::uint32_t handle = entity.record.type.value();

        if (!handle && !full_reconstruction && id) {
            const auto& candidate = touch_entity(id.value());

            if (candidate.retained_type) {
                handle = candidate.retained_type.value();
                entity.record.type = candidate.retained_type;
            }
        }

        if (!handle) {
            if (full_reconstruction) {
                handle = next_type_slot++;
            }
            else if (claimed_free_type_slots.size() < owner->free_type_slots.size()) {
                handle = owner->free_type_slots[ owner->free_type_slots.size() - 1 -
                        claimed_free_type_slots.size()];

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
        auto& aggregate = contributions->touch_entity(id);

        if (!aggregate.declarations &&
            !aggregate.aggregate_declarations &&
            !aggregate.aggregate_definitions) {
            if (slot.entity.record.live() && slot.entity.record.type) {
                auto& type = touch_type(slot.entity.record.type.value());
                type.kind = candidate_type_kind::removed;
                type.value.reset();
            }

            // stable_id reservations are historical Project identity. Removing an
            // Entity tombstones only its hot Entry; identity[name] remains mapped
            // so later resurrection reuses the same stable_id.
            slot.entity.record = {};
            return {};
        }

        auto type = std::make_unique<graph::type_storage>();
        slot.entity.record.name = name;
        touch_identity(name.value()).value = id;

        if (aggregate.aggregate_declarations || aggregate.aggregate_definitions) {
            const bool defined = aggregate.aggregate_definitions != 0;

            type->record.kind = user_type_kind::aggregate;
            type->record.definition = {};

            auto build = std::make_unique<graph::type_build_state>();
            build->definition_pending = defined;

            if (slot.entity.record.type &&
                slot.entity.record.type.value() < owner->candidate_types.size()) {
                auto& old = owner->candidate_types[slot.entity.record.type.value()];

                if (old.generation == candidate_generation &&
                    old.kind == candidate_type_kind::replacement &&
                    old.value && old.build &&
                    old.value->record.kind == user_type_kind::aggregate) {
                    build->members = std::move(old.build->members);
                    build->pending_members = std::move(old.build->pending_members);
                    build->pending_modifiers = std::move(old.build->pending_modifiers);
                }
            }

            slot.entity.record.kind = entity_kind::aggregate_type;
            auto result = assign_type(id, slot.entity, std::move(type));
            if (!result.ok()) {
                return result;
            }
            owner->candidate_types[slot.entity.record.type.value()].build = std::move(build);
            return {};
        }

        const bool defined = aggregate.definitions != 0;
        const auto underlying =
            defined ? aggregate.definition->underlying : aggregate.active_type;

        type->record.kind = user_type_kind::enumeration;
        type->record.enumeration = {
            aggregate.scoped != 0,
            aggregate.fixed != 0,
            underlying
        };
        type->record.definition = {};

        auto build = std::make_unique<graph::type_build_state>();
        build->definition_pending = defined;

        if (defined) {
            build->enumerators = aggregate.definition->values;
        }

        slot.entity.record.kind = entity_kind::enum_type;
        auto result = assign_type(id, slot.entity, std::move(type));
        if (!result.ok()) {
            return result;
        }
        owner->candidate_types[slot.entity.record.type.value()].build = std::move(build);
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status graph_update::begin_source_replacement(source_id source) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (!owner || !contributions || prepared || committed || !source) {
        return failure = {status_code::invalid_state};
    }

    if (contributions->was_replaced(source)) {
        return {};
    }

    try {
        const auto* previous = contributions->committed(source);
        source_contribution_state* candidate = nullptr;

        auto result = contributions->replace(source, candidate);
        if (!result.ok() || !candidate) {
            return failure = result.ok()
                ? status{status_code::initialization_failed}
                : result;
        }

        changed_sources.push_back(source.value());

        if (previous && !full_reconstruction) {
            for (const auto& contribution : previous->named) {
                result = remove_delta(source, contribution);

                if (!result.ok()) {
                    return failure = result;
                }
            }

            for (auto handle : previous->anonymous_types) {
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

status graph_update::declare_named_enum(string_id name, source_id source, const enum_build_data& data,
    stable_id& entity, type_handle& type) noexcept {

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

        source_contribution_record contribution;
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

        contributions->candidate(source)->named.push_back(contribution);

        result = materialize(id, name);

        if (!result.ok()) {
            return failure = result;
        }

        entity = id;

        type = touch_entity(id.value()) .entity.record.type;

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::declare_named_type(string_id name, source_id source, aggregate_definition_state state,
    stable_id& entity, type_handle& type) noexcept {

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

        source_contribution_record contribution;
        contribution.entity = id;
        contribution.name = name;
        contribution.kind = entity_kind::aggregate_type;

        contribution.state = state == aggregate_definition_state::defined ? enum_definition_state::defined
                : enum_definition_state::opaque;

        result = add_delta(source, id, contribution);

        if (!result.ok()) {
            return failure = result;
        }

        contributions->candidate(source)->named.push_back(contribution);

        result = materialize(id, name);

        if (!result.ok()) {
            return failure = result;
        }

        entity = id;

        type = touch_entity(id.value()) .entity.record.type;

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::add_anonymous_enum(source_id source, const enum_build_data& data,
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

        source_contribution_record contribution;

        result = build_contribution(data, contribution);

        if (!result.ok() || data.definition_state != enum_definition_state::defined) {
            return failure = result.ok() ? status{
                          status_code::configuration_failed }
                    : result;
        }

        auto storage = std::make_unique< graph::type_storage>();

        storage->record.kind = user_type_kind::enumeration;
        storage->record.enumeration = {
            data.scoped,
            contribution.fixed,
            contribution.underlying
        };
        storage->record.definition = {};

        auto build = std::make_unique<graph::type_build_state>();
        build->definition_pending = true;
        build->enumerators = contribution.definition->values;

        std::uint32_t handle = 0;

        if (claimed_free_type_slots.size() < owner->free_type_slots.size()) {
            handle = owner->free_type_slots[ owner->free_type_slots.size() - 1 -
                    claimed_free_type_slots.size()];

            claimed_free_type_slots.push_back(handle);
        }
        else {
            handle = next_type_slot++;
        }

        auto& candidate = touch_type(handle);

        candidate.kind = candidate_type_kind::replacement;

        candidate.value = std::move(storage);
        candidate.build = std::move(build);

        contributions->candidate(source)->anonymous_types.push_back(handle);

        type = type_handle{handle};

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

const entity_entry* graph_update::find(stable_id id) const noexcept {

    if (!id || !owner || prepared || committed) {
        return nullptr;
    }

    if (id.value() < owner->candidate_entities.size()) {
        const auto& slot = owner->candidate_entities[id.value()];

        if (slot.generation == candidate_generation) {
            return slot.entity.record.live() ? &slot.entity.record : nullptr;
        }
    }

    return full_reconstruction ? nullptr : owner->find(id);
}

const entity_entry* graph_update::find(string_id name) const noexcept {

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

const type_entry* graph_update::find(type_handle handle) const noexcept {

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
                return &slot.value->record;
            }
        }
    }

    return full_reconstruction ? nullptr : owner->find(handle);
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

            if (slot.kind == candidate_type_kind::replacement && slot.build) {
                return slot.build->enumerators;
            }
        }
    }

    return full_reconstruction ? std::span<const enum_value_record>{} : owner->enum_values(handle);
}

status graph_update::remove_named_entity_for_testing(stable_id id) noexcept {

    if (!id || !find(id)) {
        return {status_code::invalid_state};
    }

    try {
        for (auto source : changed_sources) {
            auto* source_state = contributions->candidate(source_id{source});
            if (!source_state) {
                return failure = {status_code::invalid_state};
            }
            auto& source_contributions = source_state->named;

            for (auto position = source_contributions.begin();
                 position != source_contributions.end();) {
                if (position->entity == id) {
                    const auto contribution = *position;

                    position = source_contributions.erase(position);

                    const auto result = remove_delta(source_id{source}, contribution);

                    if (!result.ok()) {
                        return failure = result;
                    }
                }
                else {
                    ++position;
                }
            }
        }

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status graph_update::resolve_pending_members(
    graph::type_build_state& build) noexcept {

    if (build.pending_members.empty()) {
        return {};
    }

    try {
        std::vector<member_record> resolved_members;
        resolved_members.reserve(build.pending_members.size());

        for (const auto& member : build.pending_members) {
            TypeRef resolved;

            if (member.builtin) {
                const auto raw =
                    static_cast<std::uint32_t>(*member.builtin) + 1;

                if (raw >= owner->canonical_types.size()) {
                    return failure =
                        {status_code::configuration_failed};
                }

                resolved = TypeRef{raw};
            }
            else {
                const auto* entity =
                    find(member.user_type_name);

                if (!entity) {
                    // The source-language meaning is already known. This is only
                    // an unresolved canonical dependency and is legal until this
                    // final construction barrier.
                    return failure =
                        {status_code::configuration_failed};
                }

                auto result =
                    get_or_create_named_type_ref(
                        entity->type,
                        resolved);

                if (!result.ok()) {
                    return result;
                }
            }

            const auto begin =
                static_cast<std::size_t>(
                    member.modifier_offset);

            const auto count =
                static_cast<std::size_t>(
                    member.modifier_count);

            if (begin > build.pending_modifiers.size() ||
                count > build.pending_modifiers.size() - begin) {
                return failure =
                    {status_code::configuration_failed};
            }

            for (std::size_t index = 0;
                 index < count;
                 ++index) {
                const auto& modifier =
                    build.pending_modifiers[begin + index];

                TypeRef next;
                auto result =
                    get_or_create_derived(
                        modifier.kind,
                        resolved,
                        modifier.payload,
                        next);

                if (!result.ok()) {
                    return result;
                }

                resolved = next;
            }

            resolved_members.push_back({
                member.name,
                resolved
            });
        }

        build.members = std::move(resolved_members);
        build.pending_members.clear();
        build.pending_modifiers.clear();
        return {};
    }
    catch (...) {
        return failure =
            {status_code::initialization_failed};
    }
}


status graph_update::validate_live_member_type_refs() noexcept {
    try {
        const auto canonical_count =
            owner->canonical_types.size() +
            added_canonical_types.size();

        const auto canonical_record =
            [&](TypeRef type) noexcept
                -> const graph::canonical_type_record* {
                if (!type) {
                    return nullptr;
                }

                const auto raw =
                    static_cast<std::size_t>(type.value());

                if (raw < owner->canonical_types.size()) {
                    return &owner->canonical_types[raw];
                }

                const auto offset =
                    raw - owner->canonical_types.size();

                return offset < added_canonical_types.size()
                    ? &added_canonical_types[offset]
                    : nullptr;
            };

        const auto type_is_live =
            [&](type_handle handle) noexcept {
                if (!handle) {
                    return false;
                }

                if (handle.value() <
                    owner->candidate_types.size()) {
                    const auto& candidate =
                        owner->candidate_types[handle.value()];

                    if (candidate.generation ==
                        candidate_generation) {
                        if (candidate.kind ==
                            candidate_type_kind::removed) {
                            return false;
                        }

                        if (candidate.kind ==
                            candidate_type_kind::replacement) {
                            return candidate.value != nullptr;
                        }
                    }
                }

                return !full_reconstruction &&
                    handle.value() <= owner->types.size() &&
                    owner->types[handle.value() - 1] != nullptr;
            };

        std::size_t visited_type_refs = 0;

        const auto type_ref_is_live =
            [&](TypeRef initial) noexcept {
                auto current = initial;

                for (std::size_t depth = 0;
                     depth < canonical_count;
                     ++depth) {
                    const auto* record =
                        canonical_record(current);

                    if (!record) {
                        return false;
                    }

                    ++visited_type_refs;

                    switch (record->kind) {
                    case canonical_type_kind::builtin:
                        return true;

                    case canonical_type_kind::named:
                        return type_is_live(record->named);

                    case canonical_type_kind::derived:
                        current = record->derived.child;
                        break;

                    default:
                        return false;
                    }
                }

                return false;
            };

        std::size_t visited_types = 0;
        std::size_t dependency_edges = 0;

        const auto validate_handle =
            [&](std::uint32_t raw_handle) noexcept -> status {
                const graph::type_storage* storage = nullptr;
                bool replacement = false;

                if (raw_handle <
                    owner->candidate_types.size()) {
                    const auto& candidate =
                        owner->candidate_types[raw_handle];

                    if (candidate.generation ==
                        candidate_generation) {
                        if (candidate.kind ==
                            candidate_type_kind::removed) {
                            return {};
                        }

                        if (candidate.kind ==
                            candidate_type_kind::replacement) {
                            storage = candidate.value.get();
                            replacement = true;
                        }
                    }
                }

                if (!storage) {
                    if (full_reconstruction ||
                        raw_handle > owner->types.size() ||
                        !owner->types[raw_handle - 1]) {
                        return {};
                    }

                    storage =
                        owner->types[raw_handle - 1].get();
                }

                if (storage->record.kind !=
                    user_type_kind::aggregate) {
                    return {};
                }

                if (replacement) {
                    const auto& candidate =
                        owner->candidate_types[raw_handle];

                    if (!candidate.build ||
                        !candidate.build->pending_members.empty() ||
                        !candidate.build->pending_modifiers.empty()) {
                        return {
                            status_code::configuration_failed
                        };
                    }

                    for (const auto& member :
                         candidate.build->members) {
                        if (!type_ref_is_live(member.type)) {
                            return {
                                status_code::configuration_failed
                            };
                        }
                    }

                    return {};
                }

                if (!storage->record.definition) {
                    return {};
                }

                const auto begin =
                    static_cast<std::size_t>(
                        storage->record.definition.begin - 1);
                const auto count =
                    static_cast<std::size_t>(
                        storage->record.definition.count);

                if (begin > owner->member_records.size() ||
                    count >
                        owner->member_records.size() - begin) {
                    return {
                        status_code::configuration_failed
                    };
                }

                for (std::size_t index = 0;
                     index < count;
                     ++index) {
                    if (!type_ref_is_live(
                            owner->member_records[
                                begin + index].type)) {
                        return {
                            status_code::configuration_failed
                        };
                    }
                }

                return {};
            };

        if (full_reconstruction) {
            const auto handle_count =
                owner->candidate_types.empty()
                    ? std::size_t{0}
                    : owner->candidate_types.size() - 1;

            for (std::uint32_t handle = 1;
                 handle <= handle_count;
                 ++handle) {
                const auto& candidate =
                    owner->candidate_types[handle];

                if (candidate.generation !=
                        candidate_generation ||
                    candidate.kind !=
                        candidate_type_kind::replacement ||
                    !candidate.value) {
                    continue;
                }

                ++visited_types;

                const auto result =
                    validate_handle(handle);

                if (!result.ok()) {
                    return failure = result;
                }
            }
        }
        else {
            const auto handle_count =
                (std::max)(
                    owner->types.size(),
                    owner->candidate_types.empty()
                        ? std::size_t{0}
                        : owner->candidate_types.size() - 1);

            std::unordered_set<std::uint32_t> visited;
            visited.reserve(
                changed_types.size() * 2 + 8);

            std::vector<std::uint32_t> work;
            work.reserve(changed_types.size());

            for (const auto handle : changed_types) {
                if (handle == 0 ||
                    handle > handle_count ||
                    !visited.insert(handle).second) {
                    continue;
                }

                work.push_back(handle);
            }

            for (std::size_t position = 0;
                 position < work.size();
                 ++position) {
                const auto handle = work[position];
                ++visited_types;

                const auto result =
                    validate_handle(handle);

                if (!result.ok()) {
                    return failure = result;
                }

                if (handle >=
                    owner->reverse_type_dependents.size()) {
                    continue;
                }

                for (const auto dependent :
                     owner->reverse_type_dependents[handle]) {
                    ++dependency_edges;

                    if (dependent == 0 ||
                        dependent > handle_count ||
                        !visited.insert(dependent).second) {
                        continue;
                    }

                    work.push_back(dependent);
                }
            }
        }

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
        storage_telemetry.validation_visited_types =
            visited_types;
        storage_telemetry.validation_visited_type_refs =
            visited_type_refs;
        storage_telemetry.validation_dependency_edges =
            dependency_edges;
#endif

        return {};
    }
    catch (...) {
        return failure = {
            status_code::initialization_failed
        };
    }
}

status graph_update::prepare_dependency_index_updates() noexcept {
    if (full_reconstruction) {
        return {};
    }

    try {
        prepared_type_dependency_updates.clear();
        prepared_reverse_dependency_updates.clear();

        const auto canonical_record =
            [&](TypeRef type) noexcept
                -> const graph::canonical_type_record* {
                if (!type) {
                    return nullptr;
                }

                const auto raw =
                    static_cast<std::size_t>(type.value());

                if (raw < owner->canonical_types.size()) {
                    return &owner->canonical_types[raw];
                }

                const auto offset =
                    raw - owner->canonical_types.size();

                return offset < added_canonical_types.size()
                    ? &added_canonical_types[offset]
                    : nullptr;
            };

        const auto collect =
            [&](std::span<const member_record> members,
                std::vector<std::uint32_t>& output) -> status {
                output.clear();

                for (const auto& member : members) {
                    auto current = member.type;
                    type_handle named_base;

                    for (std::size_t depth = 0;
                         depth <
                            owner->canonical_types.size() +
                                added_canonical_types.size();
                         ++depth) {
                        const auto* record =
                            canonical_record(current);

                        if (!record) {
                            return {
                                status_code::configuration_failed
                            };
                        }

                        if (record->kind ==
                            canonical_type_kind::builtin) {
                            break;
                        }

                        if (record->kind ==
                            canonical_type_kind::named) {
                            named_base = record->named;
                            break;
                        }

                        if (record->kind !=
                            canonical_type_kind::derived) {
                            return {
                                status_code::configuration_failed
                            };
                        }

                        current = record->derived.child;
                    }

                    if (named_base &&
                        std::find(
                            output.begin(),
                            output.end(),
                            named_base.value()) ==
                            output.end()) {
                        output.push_back(named_base.value());
                    }
                }

                return {};
            };

        const auto reverse_update =
            [&](std::uint32_t dependency)
                -> std::vector<std::uint32_t>& {
                for (auto& update :
                     prepared_reverse_dependency_updates) {
                    if (update.handle == dependency) {
                        return update.values;
                    }
                }

                dependency_list_update update;
                update.handle = dependency;

                if (dependency <
                    owner->reverse_type_dependents.size()) {
                    update.values =
                        owner->reverse_type_dependents[
                            dependency];
                }

                prepared_reverse_dependency_updates.push_back(
                    std::move(update));

                return prepared_reverse_dependency_updates.back().values;
            };

        for (const auto handle : changed_types) {
            if (!handle) {
                continue;
            }

            std::vector<std::uint32_t> old_dependencies;

            if (handle <
                owner->type_dependencies.size()) {
                old_dependencies =
                    owner->type_dependencies[handle];
            }

            std::vector<std::uint32_t> new_dependencies;

            if (handle <
                owner->candidate_types.size()) {
                const auto& candidate =
                    owner->candidate_types[handle];

                if (candidate.generation ==
                        candidate_generation &&
                    candidate.kind ==
                        candidate_type_kind::replacement &&
                    candidate.value &&
                    candidate.value->record.kind ==
                        user_type_kind::aggregate &&
                    candidate.build &&
                    candidate.build->definition_pending) {
                    const auto result =
                        collect(
                            candidate.build->members,
                            new_dependencies);

                    if (!result.ok()) {
                        return failure = result;
                    }
                }
                else if (candidate.generation !=
                             candidate_generation ||
                         candidate.kind ==
                             candidate_type_kind::unchanged) {
                    new_dependencies = old_dependencies;
                }
            }
            else {
                new_dependencies = old_dependencies;
            }

            prepared_type_dependency_updates.push_back({
                handle,
                new_dependencies
            });

            std::vector<std::uint32_t> affected =
                old_dependencies;

            for (const auto dependency :
                 new_dependencies) {
                if (std::find(
                        affected.begin(),
                        affected.end(),
                        dependency) ==
                    affected.end()) {
                    affected.push_back(dependency);
                }
            }

            for (const auto dependency : affected) {
                auto& dependents =
                    reverse_update(dependency);

                dependents.erase(
                    std::remove(
                        dependents.begin(),
                        dependents.end(),
                        handle),
                    dependents.end());

                if (std::find(
                        new_dependencies.begin(),
                        new_dependencies.end(),
                        dependency) !=
                    new_dependencies.end() &&
                    std::find(
                        dependents.begin(),
                        dependents.end(),
                        handle) ==
                    dependents.end()) {
                    dependents.push_back(handle);
                }
            }
        }

        return {};
    }
    catch (...) {
        return failure = {
            status_code::initialization_failed
        };
    }
}

status graph_update::build_rebuild_dependency_index() noexcept {
    if (!full_reconstruction) {
        return {};
    }

    try {
        rebuilt_type_dependencies.clear();
        rebuilt_reverse_type_dependents.clear();

        rebuilt_type_dependencies.resize(
            rebuilt_types.size() + 1);
        rebuilt_reverse_type_dependents.resize(
            rebuilt_types.size() + 1);

        const auto named_base =
            [&](TypeRef initial,
                type_handle& output) noexcept {
                output = {};
                auto current = initial;

                for (std::size_t depth = 0;
                     depth < rebuilt_canonical_types.size();
                     ++depth) {
                    if (!current ||
                        current.value() >=
                            rebuilt_canonical_types.size()) {
                        return false;
                    }

                    const auto& record =
                        rebuilt_canonical_types[
                            current.value()];

                    if (record.kind ==
                        canonical_type_kind::builtin) {
                        return true;
                    }

                    if (record.kind ==
                        canonical_type_kind::named) {
                        output = record.named;
                        return static_cast<bool>(output);
                    }

                    if (record.kind !=
                        canonical_type_kind::derived) {
                        return false;
                    }

                    current = record.derived.child;
                }

                return false;
            };

        for (std::uint32_t handle = 1;
             handle <= rebuilt_types.size();
             ++handle) {
            const auto* storage =
                rebuilt_types[handle - 1].get();

            if (!storage ||
                storage->record.kind !=
                    user_type_kind::aggregate ||
                !storage->record.definition) {
                continue;
            }

            const auto begin =
                static_cast<std::size_t>(
                    storage->record.definition.begin - 1);
            const auto count =
                static_cast<std::size_t>(
                    storage->record.definition.count);

            if (begin > rebuilt_member_records.size() ||
                count >
                    rebuilt_member_records.size() - begin) {
                return failure = {
                    status_code::configuration_failed
                };
            }

            auto& dependencies =
                rebuilt_type_dependencies[handle];

            for (std::size_t index = 0;
                 index < count;
                 ++index) {
                type_handle dependency;

                if (!named_base(
                        rebuilt_member_records[
                            begin + index].type,
                        dependency)) {
                    return failure = {
                        status_code::configuration_failed
                    };
                }

                if (dependency &&
                    std::find(
                        dependencies.begin(),
                        dependencies.end(),
                        dependency.value()) ==
                        dependencies.end()) {
                    dependencies.push_back(
                        dependency.value());
                }
            }

            for (const auto dependency :
                 dependencies) {
                if (dependency >=
                    rebuilt_reverse_type_dependents.size()) {
                    return failure = {
                        status_code::configuration_failed
                    };
                }

                rebuilt_reverse_type_dependents[
                    dependency].push_back(handle);
            }
        }

        return {};
    }
    catch (...) {
        return failure = {
            status_code::initialization_failed
        };
    }
}

status graph_update::prepare_full_reconstruction() noexcept {
    // G0 is a detached rebuild. Historical canonical name -> stable_id mappings
    // remain available through the Project identity namespace, but no committed
    // Entity, type slot, definition slice, or TypeRef participates as current
    // canonical state. Missing Sources therefore disappear naturally from G0.
    return {};
}

// Materializes the complete G0 canonical storage into detached arrays. The
// rebuild path never edits committed Entity/Type storage in place: all current
// live state is reconstructed from candidate Source contributions and swapped at
// publication. Historical stable_id/name reservations remain in their Project
// namespaces, but stale committed payload is not copied into G0.
status graph_update::build_rebuild_storage() noexcept {
    if (!full_reconstruction) {
        return {};
    }

    try {
        rebuilt_identity = owner->identity;
        if (rebuilt_identity.size() < owner->candidate_identities.size()) {
            rebuilt_identity.resize(owner->candidate_identities.size());
        }

        for (auto name : changed_identities) {
            if (name >= owner->candidate_identities.size() ||
                owner->candidate_identities[name].generation != candidate_generation) {
                return failure = {status_code::configuration_failed};
            }

            rebuilt_identity[name] = owner->candidate_identities[name].value;
        }

        rebuilt_entities.clear();
        rebuilt_entities.resize(next_stable_id);
        rebuilt_entity_count = 0;

        for (std::uint32_t id = 1; id < next_stable_id; ++id) {
            if (id >= owner->candidate_entities.size()) {
                continue;
            }

            const auto& candidate = owner->candidate_entities[id];
            if (candidate.generation != candidate_generation ||
                !candidate.entity.record.live()) {
                continue;
            }

            rebuilt_entities[id] = candidate.entity;
            ++rebuilt_entity_count;
        }

        rebuilt_types.clear();
        rebuilt_types.resize(next_type_slot > 0 ? next_type_slot - 1 : 0);
        rebuilt_free_type_slots.clear();
        rebuilt_type_count = 0;

        for (std::uint32_t handle = 1; handle < next_type_slot; ++handle) {
            if (handle >= owner->candidate_types.size()) {
                rebuilt_free_type_slots.push_back(handle);
                continue;
            }

            const auto& candidate = owner->candidate_types[handle];
            if (candidate.generation != candidate_generation ||
                candidate.kind != candidate_type_kind::replacement ||
                !candidate.value) {
                rebuilt_free_type_slots.push_back(handle);
                continue;
            }

            rebuilt_types[handle - 1] =
                std::make_unique<graph::type_storage>(*candidate.value);
            ++rebuilt_type_count;
        }

        for (std::uint32_t id = 1; id < rebuilt_entities.size(); ++id) {
            const auto& entity = rebuilt_entities[id].record;
            if (!entity.live()) {
                continue;
            }

            const auto handle = entity.type.value();
            if (!handle || handle > rebuilt_types.size() ||
                !rebuilt_types[handle - 1]) {
                return failure = {status_code::configuration_failed};
            }
        }

        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

// Rebuilds the generation-local canonical TypeRef namespace for G0.
// Existing incremental generations keep TypeRef values append-only; an explicit
// Rebuild may compact/reindex them because no TypeRef is a persistent Project ID.
status graph_update::rebuild_canonical_type_table() noexcept {

    if (!full_reconstruction) {
        return {};
    }

    try {
        rebuilt_canonical_types.clear();
        rebuilt_named_type_refs.clear();
        rebuilt_derived_type_index.clear();

        const auto combined_count =
            owner->canonical_types.size() +
            added_canonical_types.size();

        rebuilt_canonical_types.reserve(combined_count);
        rebuilt_canonical_types.push_back({});

        for (std::uint32_t value = 0;
             value <= static_cast<std::uint32_t>(builtin_type::void_type);
             ++value) {
            graph::canonical_type_record record;
            record.kind = canonical_type_kind::builtin;
            record.builtin = static_cast<builtin_type>(value);
            rebuilt_canonical_types.push_back(record);
        }

        rebuilt_named_type_refs.resize(next_type_slot);
        rebuilt_derived_type_index.reserve(
            owner->derived_type_index.size() +
            added_derived_type_index.size());

        const auto type_is_live =
            [&](std::uint32_t handle) noexcept {
                if (handle == 0) {
                    return false;
                }

                if (handle < owner->candidate_types.size()) {
                    const auto& candidate =
                        owner->candidate_types[handle];

                    if (candidate.generation == candidate_generation) {
                        if (candidate.kind == candidate_type_kind::removed) {
                            return false;
                        }

                        if (candidate.kind == candidate_type_kind::replacement) {
                            return candidate.value != nullptr;
                        }
                    }
                }

                return handle <= owner->types.size() &&
                    owner->types[handle - 1] != nullptr;
            };

        // Every live user type gets exactly one named TypeRef, even when the
        // current definitions do not reference it. This keeps future incremental
        // resolve_type() O(1) without retaining historical dead named refs.
        for (std::uint32_t handle = 1;
             handle < next_type_slot;
             ++handle) {
            if (!type_is_live(handle)) {
                continue;
            }

            const auto raw = rebuilt_canonical_types.size();

            if (raw >
                (std::numeric_limits<std::uint32_t>::max)()) {
                return failure = {
                    status_code::initialization_failed
                };
            }

            const TypeRef type{
                static_cast<std::uint32_t>(raw)
            };

            graph::canonical_type_record record;
            record.kind = canonical_type_kind::named;
            record.named = type_handle{handle};

            rebuilt_canonical_types.push_back(record);
            rebuilt_named_type_refs[handle] = type;
        }

        std::vector<TypeRef> remap(combined_count);

        const auto canonical_record =
            [&](TypeRef type) noexcept
                -> const graph::canonical_type_record* {
                if (!type) {
                    return nullptr;
                }

                const auto raw =
                    static_cast<std::size_t>(type.value());

                if (raw < owner->canonical_types.size()) {
                    return &owner->canonical_types[raw];
                }

                const auto offset =
                    raw - owner->canonical_types.size();

                return offset < added_canonical_types.size()
                    ? &added_canonical_types[offset]
                    : nullptr;
            };

        const auto remap_type =
            [&](auto&& self, TypeRef old_type, TypeRef& output) -> status {
                output = {};

                if (!old_type ||
                    old_type.value() >= remap.size()) {
                    return {
                        status_code::configuration_failed
                    };
                }

                if (remap[old_type.value()]) {
                    output = remap[old_type.value()];
                    return {};
                }

                const auto* record =
                    canonical_record(old_type);

                if (!record) {
                    return {
                        status_code::configuration_failed
                    };
                }

                switch (record->kind) {
                case canonical_type_kind::builtin: {
                    const auto builtin_value =
                        static_cast<std::uint32_t>(record->builtin);

                    if (builtin_value >
                        static_cast<std::uint32_t>(builtin_type::void_type)) {
                        return {
                            status_code::configuration_failed
                        };
                    }

                    output = TypeRef{builtin_value + 1};
                    break;
                }

                case canonical_type_kind::named: {
                    const auto handle = record->named.value();

                    if (!handle ||
                        handle >= rebuilt_named_type_refs.size() ||
                        !rebuilt_named_type_refs[handle]) {
                        return {
                            status_code::configuration_failed
                        };
                    }

                    output = rebuilt_named_type_refs[handle];
                    break;
                }

                case canonical_type_kind::derived: {
                    TypeRef child;
                    auto result =
                        self(self, record->derived.child, child);

                    if (!result.ok()) {
                        return result;
                    }

                    const graph::derived_type_key key{
                        record->derived.kind,
                        child,
                        record->derived.payload
                    };

                    if (const auto existing =
                            rebuilt_derived_type_index.find(key);
                        existing != rebuilt_derived_type_index.end()) {
                        output = existing->second;
                        break;
                    }

                    const auto raw =
                        rebuilt_canonical_types.size();

                    if (raw >
                        (std::numeric_limits<std::uint32_t>::max)()) {
                        return {
                            status_code::initialization_failed
                        };
                    }

                    output = TypeRef{
                        static_cast<std::uint32_t>(raw)
                    };

                    graph::canonical_type_record rebuilt;
                    rebuilt.kind = canonical_type_kind::derived;
                    rebuilt.derived = {
                        record->derived.kind,
                        child,
                        record->derived.payload
                    };

                    rebuilt_canonical_types.push_back(rebuilt);
                    rebuilt_derived_type_index.emplace(key, output);
                    break;
                }

                default:
                    return {
                        status_code::configuration_failed
                    };
                }

                remap[old_type.value()] = output;
                return {};
            };

        // Only committed semantic references need to survive G0. Remapping the
        // resolved candidate member types also defines the reachability set for
        // derived TypeRefs, so historical unused derived chains disappear.
        for (std::uint32_t handle = 1;
             handle < next_type_slot;
             ++handle) {
            if (handle >= owner->candidate_types.size()) {
                continue;
            }

            auto& candidate =
                owner->candidate_types[handle];

            if (candidate.generation != candidate_generation ||
                candidate.kind != candidate_type_kind::replacement ||
                !candidate.value ||
                candidate.value->record.kind != user_type_kind::aggregate ||
                !candidate.build) {
                continue;
            }

            for (auto& member : candidate.build->members) {
                TypeRef rebuilt;
                const auto result =
                    remap_type(remap_type, member.type, rebuilt);

                if (!result.ok()) {
                    return failure = result;
                }

                member.type = rebuilt;
            }
        }

        return {};
    }
    catch (...) {
        return failure = {
            status_code::initialization_failed
        };
    }
}

status graph_update::collect_rebuild_string_retention(
    std::size_t candidate_string_slots,
    std::vector<std::uint8_t>& retained) const noexcept {

    retained.clear();

    if (!full_reconstruction) {
        return {};
    }

    try {
        retained.resize(candidate_string_slots + 1, 0);

        const auto mark =
            [&](string_id id) noexcept -> bool {
                if (!id || id.value() > candidate_string_slots) {
                    return false;
                }

                retained[id.value()] = 1;
                return true;
            };

        // Historical canonical name reservations are persistent Project identity.
        // Their spelling must remain available even when the Entity is currently
        // dead so a later reappearance can recover the same stable_id.
        const auto identity_slots =
            (std::max)(
                owner->identity.size(),
                owner->candidate_identities.size());

        for (std::size_t name = 1;
             name < identity_slots;
             ++name) {
            stable_id entity;

            if (name < owner->candidate_identities.size()) {
                const auto& candidate =
                    owner->candidate_identities[name];

                if (candidate.generation == candidate_generation) {
                    entity = candidate.value;
                }
            }

            if (!entity && name < owner->identity.size()) {
                entity = owner->identity[name];
            }

            if (entity) {
                if (name > candidate_string_slots) {
                    return {status_code::configuration_failed};
                }

                retained[name] = 1;
            }
        }

        // G0 has already built compact current definition arenas. Their local
        // member/enumerator spellings are the only non-identity strings that
        // must survive into the committed canonical Graph.
        for (const auto& member : rebuilt_member_records) {
            if (!mark(member.name)) {
                return {status_code::configuration_failed};
            }
        }

        for (const auto& value : rebuilt_enum_value_records) {
            if (!mark(value.name)) {
                return {status_code::configuration_failed};
            }
        }

        return {};
    }
    catch (...) {
        retained.clear();
        return {status_code::initialization_failed};
    }
}

status graph_update::canonicalize_new_stable_ids(
    const string_registry_update& strings) noexcept {

    const auto base = owner->next_stable_id;

    if (next_stable_id <= base) {
        return {};
    }

    try {
        const auto provisional_count =
            static_cast<std::size_t>(
                next_stable_id - base);

        std::vector<string_id> names(
            provisional_count);

        // Each surviving new canonical Entity has at least one source
        // contribution. Use that canonical string_id to obtain deterministic
        // lexical bytes without depending on string_id allocation order.
        for (auto source : changed_sources) {
            const auto* state_ptr =
                contributions->candidate(source_id{source});
            if (!state_ptr) {
                return failure = {status_code::invalid_state};
            }
            const auto& state = *state_ptr;

            for (const auto& contribution : state.named) {
                const auto raw =
                    contribution.entity.value();

                if (raw >= base &&
                    raw < next_stable_id) {
                    names[raw - base] =
                        contribution.name;
                }
            }
        }

        struct assignment {
            string_id name{};
            std::uint32_t provisional = 0;
            std::string_view bytes;
        };

        std::vector<assignment> assignments;
        assignments.reserve(provisional_count);

        for (std::size_t offset = 0;
             offset < names.size();
             ++offset) {
            if (!names[offset]) {
                continue;
            }

            const auto bytes =
                strings.get_for_validation(
                    names[offset]);

            if (!bytes) {
                return failure =
                    {status_code::configuration_failed};
            }

            assignments.push_back({
                names[offset],
                static_cast<std::uint32_t>(
                    base + offset),
                *bytes
            });
        }

        std::sort(
            assignments.begin(),
            assignments.end(),
            [](const assignment& left,
               const assignment& right) {
                return left.bytes < right.bytes;
            });

        std::vector<std::uint32_t> remap(
            provisional_count,
            0);

        for (std::size_t index = 0;
             index < assignments.size();
             ++index) {
            remap[
                assignments[index].provisional - base] =
                static_cast<std::uint32_t>(
                    base + index);
        }

        std::vector<graph::candidate_entity_slot>
            old_entities(provisional_count);

        for (std::size_t offset = 0;
             offset < provisional_count;
             ++offset) {
            const auto raw =
                static_cast<std::size_t>(base) +
                offset;

            if (raw <
                owner->candidate_entities.size()) {
                old_entities[offset] =
                    owner->candidate_entities[raw];

                owner->candidate_entities[raw] = {};
            }
        }

        for (const auto& item : assignments) {
            const auto final_id =
                remap[item.provisional - base];

            auto candidate =
                old_entities[
                    item.provisional - base];

            if (candidate.generation !=
                    candidate_generation ||
                !candidate.entity.record.live()) {
                return failure =
                    {status_code::configuration_failed};
            }


            if (owner->candidate_entities.size() <=
                final_id) {
                owner->candidate_entities.resize(
                    static_cast<std::size_t>(
                        final_id) + 1);
            }

            owner->candidate_entities[final_id] =
                std::move(candidate);

            auto& identity_slot =
                owner->candidate_identities[
                    item.name.value()];

            identity_slot.value =
                stable_id{final_id};
        }

        auto construction_remap =
            contributions->remap_new_entities(base, remap);
        if (!construction_remap.ok()) {
            return failure = construction_remap;
        }

        for (auto source : changed_sources) {
            auto* state_ptr =
                contributions->candidate(source_id{source});
            if (!state_ptr) {
                return failure = {status_code::invalid_state};
            }
            auto& state = *state_ptr;

            for (auto& contribution : state.named) {
                const auto raw =
                    contribution.entity.value();

                if (raw >= base &&
                    raw < base + provisional_count) {
                    const auto mapped =
                        remap[raw - base];

                    if (!mapped) {
                        return failure =
                            {status_code::configuration_failed};
                    }

                    contribution.entity =
                        stable_id{mapped};
                }
            }
        }

        std::vector<std::uint32_t>
            remapped_changed_entities;

        remapped_changed_entities.reserve(
            changed_entities.size());

        for (auto id : changed_entities) {
            if (id < base) {
                remapped_changed_entities.push_back(id);
                continue;
            }

            if (id < base + provisional_count) {
                const auto mapped =
                    remap[id - base];

                if (mapped) {
                    remapped_changed_entities.push_back(
                        mapped);
                }
            }
        }

        changed_entities =
            std::move(remapped_changed_entities);

        next_stable_id =
            static_cast<std::uint32_t>(
                base + assignments.size());

        if (owner->candidate_entities.size() >
            next_stable_id) {
            owner->candidate_entities.resize(
                next_stable_id);
        }

        return {};
    }
    catch (...) {
        return failure =
            {status_code::initialization_failed};
    }
}

// Performs every allocation-sensitive validation/reservation required before
// publish_prepared() mutates the committed Graph.
status graph_update::prepare_publish(const source_manager_update& sources,
    const string_registry_update& strings) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (!owner || prepared || committed || owner->generation != base_generation) {
        return failure = {status_code::invalid_state};
    }

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    storage_telemetry = {};
    storage_telemetry.changed_sources = changed_sources.size();
    storage_telemetry.changed_entities = changed_entities.size();
    storage_telemetry.changed_types = changed_types.size();
#endif

    auto result =
        prepare_full_reconstruction();

    if (!result.ok()) {
        return result;
    }

    result =
        canonicalize_new_stable_ids(strings);

    if (!result.ok()) {
        return result;
    }

    for (auto handle : changed_types) {
        auto& candidate =
            owner->candidate_types[handle];

        if (candidate.kind ==
                candidate_type_kind::replacement &&
            candidate.value &&
            candidate.value->record.kind ==
                user_type_kind::aggregate) {
            result =
                candidate.build
                    ? resolve_pending_members(*candidate.build)
                    : status{status_code::configuration_failed};

            if (!result.ok()) {
                return result;
            }
        }
    }

    result = validate_live_member_type_refs();

    if (!result.ok()) {
        return result;
    }

    result = rebuild_canonical_type_table();

    if (!result.ok()) {
        return result;
    }

    for (auto source : changed_sources) {
        if (!sources.contains_for_validation(source_id{source})) {
            return failure = {status_code::configuration_failed};
        }

        const auto* state = contributions->candidate(source_id{source});
        if (!state) {
            return failure = {status_code::invalid_state};
        }

        for (const auto& contribution : state->named) {
            if (!strings.get_for_validation(contribution.name)) {
                return failure = {status_code::configuration_failed};
            }

            if (contribution.definition) {
                for (const auto& value : contribution.definition->values) {
                    if (!strings.get_for_validation(value.name)) {
                        return failure = {
                            status_code::configuration_failed };
                    }
                }
            }
        }
    }

    std::size_t added_member_count = 0;
    std::size_t added_enum_value_count = 0;

    for (auto handle : changed_types) {
        const auto& candidate = owner->candidate_types[handle];

        if (candidate.kind != candidate_type_kind::replacement || !candidate.value) {
            continue;
        }

        if (candidate.value->record.kind == user_type_kind::aggregate) {
            added_member_count += candidate.build->members.size();

            for (const auto& member : candidate.build->members) {
                if (!strings.get_for_validation(member.name) || !member.type) {
                    return failure = {status_code::configuration_failed};
                }
            }
        }
        else {
            added_enum_value_count += candidate.build->enumerators.size();

            for (const auto& value : candidate.build->enumerators) {
                if (!strings.get_for_validation(value.name)) {
                    return failure = {status_code::configuration_failed};
                }
            }
        }
    }

    try {
        if (full_reconstruction) {
            rebuilt_member_records.clear();
            rebuilt_enum_value_records.clear();
            rebuilt_member_records.reserve(added_member_count);
            rebuilt_enum_value_records.reserve(added_enum_value_count);

            // Rebuild materializes every surviving definition into fresh compact
            // arenas in type-handle order. This reclaims all obsolete slices
            // accumulated by prior incremental generations while preserving the
            // O(1) one-based DefinitionRange representation.
            for (std::uint32_t handle = 1; handle < next_type_slot; ++handle) {
                if (handle >= owner->candidate_types.size()) {
                    continue;
                }

                auto& candidate = owner->candidate_types[handle];

                if (candidate.generation != candidate_generation ||
                    candidate.kind != candidate_type_kind::replacement ||
                    !candidate.value) {
                    continue;
                }

                candidate.value->record.definition = {};

                if (!candidate.build || !candidate.build->definition_pending) {
                    continue;
                }

                if (candidate.value->record.kind == user_type_kind::aggregate) {
                    candidate.value->record.definition = {
                        static_cast<std::uint32_t>(rebuilt_member_records.size() + 1),
                        static_cast<std::uint32_t>(candidate.build->members.size())
                    };

                    rebuilt_member_records.insert(
                        rebuilt_member_records.end(),
                        candidate.build->members.begin(),
                        candidate.build->members.end());
                }
                else {
                    candidate.value->record.definition = {
                        static_cast<std::uint32_t>(rebuilt_enum_value_records.size() + 1),
                        static_cast<std::uint32_t>(candidate.build->enumerators.size())
                    };

                    rebuilt_enum_value_records.insert(
                        rebuilt_enum_value_records.end(),
                        candidate.build->enumerators.begin(),
                        candidate.build->enumerators.end());
                }
            }
        }

        if (full_reconstruction) {
            result = build_rebuild_storage();
            if (!result.ok()) {
                return result;
            }

            result = build_rebuild_dependency_index();
            if (!result.ok()) {
                return result;
            }

            reserve_sparse_capacity(rebuilt_identity, rebuilt_identity.size());
            reserve_sparse_capacity(rebuilt_entities, rebuilt_entities.size());
            reserve_sparse_capacity(rebuilt_types, rebuilt_types.size());
            reserve_sparse_capacity(rebuilt_member_records, rebuilt_member_records.size());
            reserve_sparse_capacity(rebuilt_enum_value_records, rebuilt_enum_value_records.size());
            reserve_sparse_capacity(rebuilt_canonical_types, rebuilt_canonical_types.size());
            reserve_sparse_capacity(rebuilt_named_type_refs, rebuilt_named_type_refs.size());
            reserve_sparse_capacity(rebuilt_type_dependencies, rebuilt_type_dependencies.size());
            reserve_sparse_capacity(
                rebuilt_reverse_type_dependents,
                rebuilt_reverse_type_dependents.size());
        }
        else {
            result = prepare_dependency_index_updates();

            if (!result.ok()) {
                return result;
            }
            prepared_identity_size = owner->identity.size();
            prepared_entities_size = owner->entities.size();
            prepared_types_size = owner->types.size();
            prepared_named_type_refs_size = owner->named_type_refs.size();
            prepared_type_dependencies_size = owner->type_dependencies.size();
            prepared_reverse_type_dependents_size =
                owner->reverse_type_dependents.size();
            prepared_owner_growth = true;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
            const auto* identity_data = owner->identity.data();
            const auto* entities_data = owner->entities.data();
            const auto* types_data = owner->types.data();
            const auto* member_data = owner->member_records.data();
            const auto* enum_data = owner->enum_value_records.data();
            const auto* canonical_data = owner->canonical_types.data();
            const auto* named_data = owner->named_type_refs.data();

            storage_telemetry.identity = growth_before(owner->identity);
            storage_telemetry.entities = growth_before(owner->entities);
            storage_telemetry.types = growth_before(owner->types);
            storage_telemetry.member_records = growth_before(owner->member_records);
            storage_telemetry.enum_value_records = growth_before(owner->enum_value_records);
            storage_telemetry.canonical_types = growth_before(owner->canonical_types);
            storage_telemetry.named_type_refs = growth_before(owner->named_type_refs);
#endif

            grow_sparse_vector(
                owner->identity,
                owner->candidate_identities.size());
            grow_sparse_vector(
                owner->entities,
                owner->candidate_entities.size());

            const auto needed_types = next_type_slot - 1;

            grow_sparse_vector(owner->types, needed_types);

            ensure_sparse_capacity(
                owner->free_type_slots,
                owner->free_type_slots.size() +
                    changed_types.size());
            ensure_sparse_capacity(
                owner->member_records,
                owner->member_records.size() +
                    added_member_count);
            ensure_sparse_capacity(
                owner->enum_value_records,
                owner->enum_value_records.size() +
                    added_enum_value_count);
            ensure_sparse_capacity(
                owner->canonical_types,
                owner->canonical_types.size() +
                    added_canonical_types.size());

            std::uint32_t maximum_named = 0;

            for (const auto& item : added_named_type_refs) {
                maximum_named =
                    (std::max)(maximum_named, item.first);
            }

            if (!added_named_type_refs.empty()) {
                grow_sparse_vector(
                    owner->named_type_refs,
                    static_cast<std::size_t>(maximum_named) + 1);
            }

            grow_sparse_vector(
                owner->type_dependencies,
                static_cast<std::size_t>(needed_types) + 1);
            grow_sparse_vector(
                owner->reverse_type_dependents,
                static_cast<std::size_t>(needed_types) + 1);

            owner->derived_type_index.reserve(
                sparse_capacity(
                    owner->derived_type_index.size() +
                    added_derived_type_index.size()));

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
            growth_after(
                owner->identity,
                identity_data,
                storage_telemetry.identity);
            growth_after(
                owner->entities,
                entities_data,
                storage_telemetry.entities);
            growth_after(
                owner->types,
                types_data,
                storage_telemetry.types);
            growth_after(
                owner->member_records,
                member_data,
                storage_telemetry.member_records);
            growth_after(
                owner->enum_value_records,
                enum_data,
                storage_telemetry.enum_value_records);
            growth_after(
                owner->canonical_types,
                canonical_data,
                storage_telemetry.canonical_types);
            growth_after(
                owner->named_type_refs,
                named_data,
                storage_telemetry.named_type_refs);
#endif
        }
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }

    prepared = true;
    return {};
}

void graph_update::publish_prepared() noexcept {

    assert(prepared && !committed && owner);

    if (full_reconstruction) {
        owner->identity.swap(rebuilt_identity);
        owner->entities.swap(rebuilt_entities);
        owner->types.swap(rebuilt_types);
        owner->free_type_slots.swap(rebuilt_free_type_slots);
        owner->member_records.swap(rebuilt_member_records);
        owner->enum_value_records.swap(rebuilt_enum_value_records);
        owner->canonical_types.swap(rebuilt_canonical_types);
        owner->named_type_refs.swap(rebuilt_named_type_refs);
        owner->derived_type_index.swap(rebuilt_derived_type_index);
        owner->type_dependencies.swap(rebuilt_type_dependencies);
        owner->reverse_type_dependents.swap(
            rebuilt_reverse_type_dependents);

        owner->entity_count_value = rebuilt_entity_count;
        owner->user_type_count_value = rebuilt_type_count;
        owner->next_stable_id = next_stable_id;
        owner->generation = 0;
        committed = true;
        return;
    }

    for (std::size_t index = 0;
         index < claimed_free_type_slots.size();
         ++index) {
        owner->free_type_slots.pop_back();
    }

    for (auto name : changed_identities) {
        owner->identity[name] = owner->candidate_identities[name].value;
    }

    for (auto id : changed_entities) {
        auto& candidate = owner->candidate_entities[id];
        auto& committed_entity = owner->entities[id];

        const bool was_live = committed_entity.record.live();
        const bool will_live = candidate.entity.record.live();

        if (was_live != will_live) {
            if (will_live) {
                ++owner->entity_count_value;
            }
            else {
                --owner->entity_count_value;
            }
        }

        committed_entity = candidate.entity;
    }

    for (auto handle : changed_types) {
        auto& candidate = owner->candidate_types[handle];

        const bool was_live = owner->types[handle - 1] != nullptr;

        const bool will_live = candidate.kind == candidate_type_kind::replacement || (candidate.kind ==
                 candidate_type_kind::unchanged && was_live);

        if (was_live != will_live) {
            if (will_live) {
                ++owner->user_type_count_value;
            }
            else {
                --owner->user_type_count_value;
            }
        }

        if (candidate.kind == candidate_type_kind::replacement) {
            candidate.value->record.definition = {};

            if (candidate.build && candidate.build->definition_pending) {
                if (candidate.value->record.kind == user_type_kind::aggregate) {
                    candidate.value->record.definition = {
                        static_cast<std::uint32_t>(owner->member_records.size() + 1),
                        static_cast<std::uint32_t>(candidate.build->members.size())
                    };

                    owner->member_records.insert(
                        owner->member_records.end(),
                        std::make_move_iterator(candidate.build->members.begin()),
                        std::make_move_iterator(candidate.build->members.end()));
                }
                else {
                    candidate.value->record.definition = {
                        static_cast<std::uint32_t>(owner->enum_value_records.size() + 1),
                        static_cast<std::uint32_t>(candidate.build->enumerators.size())
                    };

                    owner->enum_value_records.insert(
                        owner->enum_value_records.end(),
                        std::make_move_iterator(candidate.build->enumerators.begin()),
                        std::make_move_iterator(candidate.build->enumerators.end()));
                }
            }

            candidate.build.reset();
            owner->types[handle - 1].swap(candidate.value);
        }
        else if (candidate.kind == candidate_type_kind::removed) {
            owner->types[handle - 1].reset();
            candidate.build.reset();

            owner->free_type_slots.push_back(handle);
        }
    }

    owner->canonical_types.insert(
            owner->canonical_types.end(),
            added_canonical_types.begin(),
            added_canonical_types.end());

        for (const auto& item : added_named_type_refs) {
            owner->named_type_refs[item.first] = item.second;
        }

    owner->derived_type_index.merge(added_derived_type_index);

    for (auto& update : prepared_type_dependency_updates) {
        owner->type_dependencies[update.handle].swap(
            update.values);
    }

    for (auto& update : prepared_reverse_dependency_updates) {
        owner->reverse_type_dependents[update.handle].swap(
            update.values);
    }

    owner->next_stable_id = next_stable_id;
    owner->generation = owner->generation + 1;
    prepared_owner_growth = false;
    committed = true;
}

void graph_update::rollback_prepared_owner_growth() noexcept {
    if (!owner ||
        !prepared_owner_growth ||
        committed ||
        full_reconstruction) {
        return;
    }

    if (owner->identity.size() > prepared_identity_size) {
        owner->identity.resize(prepared_identity_size);
    }

    if (owner->entities.size() > prepared_entities_size) {
        owner->entities.resize(prepared_entities_size);
    }

    if (owner->types.size() > prepared_types_size) {
        owner->types.resize(prepared_types_size);
    }

    if (owner->named_type_refs.size() >
        prepared_named_type_refs_size) {
        owner->named_type_refs.resize(
            prepared_named_type_refs_size);
    }

    if (owner->type_dependencies.size() >
        prepared_type_dependencies_size) {
        owner->type_dependencies.resize(
            prepared_type_dependencies_size);
    }

    if (owner->reverse_type_dependents.size() >
        prepared_reverse_type_dependents_size) {
        owner->reverse_type_dependents.resize(
            prepared_reverse_type_dependents_size);
    }

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
