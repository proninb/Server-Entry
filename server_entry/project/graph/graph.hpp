#pragma once

#include "../../member_index.hpp"
#include "../../source_id.hpp"
#include "../../stable_id.hpp"
#include "../../status.hpp"
#include "../../string_id.hpp"
#include "../../type_handle.hpp"
#include "../language/aggregate_semantics.hpp"
#include "../language/enum_semantics.hpp"
#include "builtin_type.hpp"
#include "type_ref.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cw::server {

class graph_build_transaction;
class graph_build_transaction_test_access;
class graph_update;
class source_manager_update;
class string_registry_update;
struct compiled_graph_state;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

struct graph_vector_growth_telemetry {
    std::uint64_t prepare_ns = 0;
    std::size_t logical_entries_touched = 0;
    std::size_t required = 0;
    std::size_t size_before = 0;
    std::size_t size_after = 0;
    std::size_t capacity_before = 0;
    std::size_t capacity_after = 0;
    std::size_t relocation_payload_bytes = 0;
    bool reallocated = false;
};

struct graph_hash_growth_telemetry {
    std::uint64_t prepare_ns = 0;
    std::size_t logical_entries_touched = 0;
    std::size_t size_before = 0;
    std::size_t size_after = 0;
    std::size_t buckets_before = 0;
    std::size_t buckets_after = 0;
    bool rehashed = false;
};

struct graph_storage_prepare_telemetry {
    graph_vector_growth_telemetry identity;
    graph_vector_growth_telemetry entities;
    graph_vector_growth_telemetry enum_aggregates;
    graph_vector_growth_telemetry source_states;
    graph_vector_growth_telemetry types;
    graph_vector_growth_telemetry free_type_slots;
    graph_vector_growth_telemetry member_records;
    graph_vector_growth_telemetry canonical_types;
    graph_vector_growth_telemetry named_type_refs;
    graph_hash_growth_telemetry derived_type_index;

    std::size_t changed_identities = 0;
    std::size_t changed_entities = 0;
    std::size_t changed_types = 0;
    std::size_t changed_sources = 0;
    std::size_t added_members = 0;
    std::size_t added_type_refs = 0;
};

struct graph_vector_storage_snapshot {
    const void* data = nullptr;
    std::size_t size = 0;
    std::size_t capacity = 0;
};

struct graph_hash_storage_snapshot {
    std::size_t size = 0;
    std::size_t buckets = 0;
};

struct graph_storage_snapshot {
    graph_vector_storage_snapshot identity;
    graph_vector_storage_snapshot entities;
    graph_vector_storage_snapshot enum_aggregates;
    graph_vector_storage_snapshot source_states;
    graph_vector_storage_snapshot types;
    graph_vector_storage_snapshot free_type_slots;
    graph_vector_storage_snapshot member_records;
    graph_vector_storage_snapshot canonical_types;
    graph_vector_storage_snapshot named_type_refs;
    graph_hash_storage_snapshot derived_type_index;

    std::size_t entity_count = 0;
    std::size_t user_type_count = 0;
    std::uint32_t next_stable_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t semantic_hash = 0;
};

#endif

enum class entity_kind : std::uint8_t {
    aggregate_type,
    enum_type
};

enum class user_type_kind : std::uint8_t {
    aggregate,
    enumeration
};

struct entity_record {
    stable_id id{};
    entity_kind kind = entity_kind::enum_type;
    string_id name{};
    source_id defining_source{};
    type_handle type{};
};

struct enum_value_record {
    string_id name{};
    std::uint64_t bits = 0;
};

struct enum_value_build {
    string_id name{};
    integral_constant value{};
};

struct enum_build_data {
    enum_definition_state definition_state = enum_definition_state::defined;
    bool scoped = false;
    std::optional<builtin_type> explicit_underlying;
    std::span<const enum_value_build> enumerators;
};

struct enum_type_record {
    enum_definition_state definition_state = enum_definition_state::opaque;
    bool scoped = false;
    bool fixed_underlying = false;
    builtin_type underlying = builtin_type::integer;
    std::uint32_t enumerator_count = 0;
};

struct aggregate_type_record {
    aggregate_definition_state definition_state = aggregate_definition_state::declared;
};

struct member_record {
    string_id name{};
    TypeRef type{};
};

static_assert(sizeof(member_record) == 8);

struct member_build {
    string_id name{};
    TypeRef type{};
};

struct user_type_record {
    user_type_kind kind = user_type_kind::enumeration;
    enum_type_record enumeration{};
    aggregate_type_record aggregate{};
};

// Owns the authoritative canonical compiled state G for one Project generation.
// Stable Entity identity is persistent; type slots and canonical TypeRefs are
// generation-owned implementation state. Source-language lookup never occurs here.
class graph final {
public:
    graph();
    ~graph();

    graph(const graph&) = delete;
    graph& operator=(const graph&) = delete;
    graph(graph&&) = delete;
    graph& operator=(graph&&) = delete;

    [[nodiscard]] status initialize(abi_configuration abi = {}) noexcept;
    [[nodiscard]] graph_update begin_update() noexcept;

    [[nodiscard]] const entity_record* find(string_id name) const noexcept;
    [[nodiscard]] const entity_record* find(stable_id id) const noexcept;
    [[nodiscard]] const user_type_record* find(type_handle handle) const noexcept;

    [[nodiscard]] std::span<const enum_value_record> enum_values(type_handle handle) const noexcept;
    [[nodiscard]] std::span<const member_record> members(type_handle handle) const noexcept;

    [[nodiscard]] std::size_t member_count(type_handle handle) const noexcept {
        return members(handle).size();
    }

    [[nodiscard]] const member_record* member(type_handle handle, member_index index) const noexcept;
    [[nodiscard]] member_index find_member(type_handle handle, string_id name) const noexcept;

    [[nodiscard]] canonical_type_kind kind(TypeRef type) const noexcept;
    [[nodiscard]] bool builtin(TypeRef type, builtin_type& output) const noexcept;
    [[nodiscard]] bool named(TypeRef type, type_handle& output) const noexcept;
    [[nodiscard]] const derived_type_record* derived(TypeRef type) const noexcept;

    [[nodiscard]] std::size_t derived_type_count() const noexcept;
    [[nodiscard]] std::size_t contribution_count(source_id source) const noexcept;

    [[nodiscard]] std::size_t entity_count() const noexcept {
        return entity_count_value;
    }

    [[nodiscard]] std::size_t user_type_count() const noexcept {
        return user_type_count_value;
    }

    [[nodiscard]] abi_configuration abi() const noexcept {
        return abi_config;
    }

    [[nodiscard]] status export_compiled(compiled_graph_state& output) const noexcept;
    [[nodiscard]] status import_compiled(const compiled_graph_state& input) noexcept;
    void swap_compiled(graph& other) noexcept;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    [[nodiscard]] graph_storage_snapshot storage_snapshot_for_testing() const noexcept;
#endif

private:
    struct type_storage;
    struct definition_payload;
    struct source_contribution;
    struct source_state;
    struct enum_aggregate;
    struct entity_slot;
    struct candidate_identity_slot;
    struct candidate_entity_slot;
    struct candidate_type_slot;
    struct candidate_source_slot;

    struct canonical_type_record {
        canonical_type_kind kind = canonical_type_kind::builtin;
        builtin_type builtin = builtin_type::void_type;
        type_handle named{};
        derived_type_record derived{};
    };

    struct derived_type_key {
        derived_type_kind kind = derived_type_kind::pointer;
        TypeRef child{};
        std::uint64_t payload = 0;

        friend bool operator==(const derived_type_key&, const derived_type_key&) noexcept = default;
    };

    struct derived_type_key_hash {
        [[nodiscard]] std::size_t operator()(const derived_type_key& key) const noexcept;
    };

    friend class graph_update;
    friend class graph_build_transaction_test_access;

    std::vector<std::unique_ptr<type_storage>> types;
    std::vector<std::uint32_t> free_type_slots;

    std::vector<entity_slot> entities;
    std::vector<stable_id> identity;
    std::vector<enum_aggregate> enum_aggregates;
    std::vector<source_state> source_states;

    std::vector<member_record> member_records;

    std::vector<canonical_type_record> canonical_types;
    std::vector<TypeRef> named_type_refs;
    std::unordered_map<derived_type_key, TypeRef, derived_type_key_hash> derived_type_index;

    std::size_t entity_count_value = 0;
    std::size_t user_type_count_value = 0;
    std::uint32_t next_stable_id = 1;
    std::uint64_t generation = 0;
    abi_configuration abi_config{};

    std::vector<candidate_identity_slot> candidate_identities;
    std::vector<candidate_entity_slot> candidate_entities;
    std::vector<candidate_type_slot> candidate_types;
    std::vector<candidate_source_slot> candidate_sources;
    std::uint64_t next_candidate_generation = 1;
};

// Represents one isolated candidate mutation of Graph. All allocating and
// validating work completes in prepare_publish(); publish_prepared() is no-fail.
class graph_update final {
public:
    class source_replacement final {
    public:
        source_replacement() noexcept = default;

        source_replacement(const source_replacement&) = delete;
        source_replacement& operator=(const source_replacement&) = delete;
        source_replacement(source_replacement&&) noexcept = default;
        source_replacement& operator=(source_replacement&&) = delete;

        [[nodiscard]] status add_named_enum(
            string_id name,
            const enum_build_data& data,
            stable_id& entity,
            type_handle& type) noexcept;

        [[nodiscard]] status add_anonymous_enum(
            const enum_build_data& data,
            type_handle& type) noexcept;

        [[nodiscard]] status add_named_type(
            string_id name,
            aggregate_definition_state state,
            stable_id& entity,
            type_handle& type) noexcept;

        [[nodiscard]] status define_members(
            type_handle type,
            std::span<const member_build> members) noexcept;

        [[nodiscard]] status resolve_type(string_id name, TypeRef& output) const noexcept;
        [[nodiscard]] status get_or_create_pointer(TypeRef child, TypeRef& output) noexcept;
        [[nodiscard]] status get_or_create_array(TypeRef child, std::uint64_t extent, TypeRef& output) noexcept;
        [[nodiscard]] status get_or_create_lvalue_reference(TypeRef child, TypeRef& output) noexcept;
        [[nodiscard]] status get_or_create_rvalue_reference(TypeRef child, TypeRef& output) noexcept;
        [[nodiscard]] TypeRef builtin_type_ref(builtin_type value) const noexcept;

    private:
        friend class graph_update;

        source_replacement(graph_update& update, source_id source) noexcept
            : update(&update), source(source) {}

        graph_update* update = nullptr;
        source_id source{};
    };

    ~graph_update();

    graph_update(const graph_update&) = delete;
    graph_update& operator=(const graph_update&) = delete;
    graph_update(graph_update&& other) noexcept;
    graph_update& operator=(graph_update&&) = delete;

    [[nodiscard]] status replace_source(source_id source, source_replacement& replacement) noexcept;

    [[nodiscard]] const entity_record* find(string_id name) const noexcept;
    [[nodiscard]] const entity_record* find(stable_id id) const noexcept;
    [[nodiscard]] const user_type_record* find(type_handle handle) const noexcept;
    [[nodiscard]] std::span<const enum_value_record> enum_values(type_handle handle) const noexcept;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    [[nodiscard]] const graph_storage_prepare_telemetry& storage_prepare_telemetry() const noexcept {
        return storage_telemetry;
    }
#endif

private:
    friend class graph;
    friend class graph_build_transaction;
    friend class graph_build_transaction_test_access;

    graph_update(graph& owner, std::uint64_t generation, std::uint64_t candidate_generation) noexcept;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
public:
#endif

    [[nodiscard]] status declare_named_enum(
        string_id name,
        source_id source,
        const enum_build_data& data,
        stable_id& entity,
        type_handle& type) noexcept;

    [[nodiscard]] status declare_named_type(
        string_id name,
        source_id source,
        aggregate_definition_state state,
        stable_id& entity,
        type_handle& type) noexcept;

    [[nodiscard]] status add_anonymous_enum(
        source_id source,
        const enum_build_data& data,
        type_handle& type) noexcept;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
private:
#endif

    [[nodiscard]] status prepare_publish(
        const source_manager_update& sources,
        const string_registry_update& strings) noexcept;

    void publish_prepared() noexcept;
    void cancel() noexcept;
    void rollback_prepared_owner_growth() noexcept;

    [[nodiscard]] status begin_source_replacement(source_id source) noexcept;
    [[nodiscard]] status remove_named_entity_for_testing(stable_id id) noexcept;

    [[nodiscard]] status build_contribution(
        const enum_build_data& data,
        graph::source_contribution& output) noexcept;

    [[nodiscard]] status add_delta(
        source_id source,
        stable_id id,
        const graph::source_contribution& contribution) noexcept;

    [[nodiscard]] status remove_delta(
        source_id source,
        const graph::source_contribution& contribution) noexcept;

    [[nodiscard]] status materialize(stable_id id, string_id name) noexcept;

    [[nodiscard]] status assign_type(
        stable_id id,
        graph::entity_slot& entity,
        std::unique_ptr<graph::type_storage> type) noexcept;

    [[nodiscard]] status get_or_create_named_type_ref(type_handle handle, TypeRef& output) noexcept;

    [[nodiscard]] status get_or_create_derived(
        derived_type_kind kind,
        TypeRef child,
        std::uint64_t payload,
        TypeRef& output) noexcept;

    [[nodiscard]] graph::candidate_identity_slot& touch_identity(std::uint32_t name);
    [[nodiscard]] graph::candidate_entity_slot& touch_entity(std::uint32_t id);
    [[nodiscard]] graph::candidate_type_slot& touch_type(std::uint32_t handle);
    [[nodiscard]] graph::candidate_source_slot& touch_source(std::uint32_t source);

    graph* owner = nullptr;

    std::vector<std::unique_ptr<graph::type_storage>> owned_types;
    std::vector<std::uint32_t> changed_identities;
    std::vector<std::uint32_t> changed_entities;
    std::vector<std::uint32_t> changed_types;
    std::vector<std::uint32_t> changed_sources;
    std::vector<std::uint32_t> claimed_free_type_slots;

    std::vector<graph::canonical_type_record> added_canonical_types;
    std::vector<std::pair<std::uint32_t, TypeRef>> added_named_type_refs;
    std::unordered_map<std::uint32_t, TypeRef> added_named_type_index;
    std::unordered_map<graph::derived_type_key, TypeRef, graph::derived_type_key_hash> added_derived_type_index;

    std::uint32_t next_stable_id = 1;
    std::uint32_t next_type_slot = 1;
    std::uint64_t base_generation = 0;
    std::uint64_t candidate_generation = 0;

    status failure{};
    bool prepared = false;
    bool committed = false;

    // prepare_publish may grow committed direct-index vector sizes so publication
    // itself stays allocation-free. Failed transactions restore these logical
    // sizes; retained capacity is explicitly non-semantic.
    std::size_t prepared_identity_size = 0;
    std::size_t prepared_entities_size = 0;
    std::size_t prepared_enum_aggregates_size = 0;
    std::size_t prepared_source_states_size = 0;
    std::size_t prepared_types_size = 0;
    std::size_t prepared_named_type_refs_size = 0;
    bool prepared_owner_growth = false;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    graph_storage_prepare_telemetry storage_telemetry;
#endif
};

} // namespace cw::server
