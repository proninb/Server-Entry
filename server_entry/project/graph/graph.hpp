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
class source_contribution_cache;
class source_contribution_cache_update;
struct source_contribution_record;
struct source_contribution_state;
struct source_definition_payload;
class string_registry_update;
struct compiled_graph_state;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

struct graph_vector_growth_telemetry {
    std::size_t size_before = 0;
    std::size_t size_after = 0;
    std::size_t capacity_before = 0;
    std::size_t capacity_after = 0;
    std::size_t relocation_payload_bytes = 0;
    bool reallocated = false;
};

struct graph_storage_prepare_telemetry {
    graph_vector_growth_telemetry identity;
    graph_vector_growth_telemetry entities;
    graph_vector_growth_telemetry types;
    graph_vector_growth_telemetry member_records;
    graph_vector_growth_telemetry enum_value_records;
    graph_vector_growth_telemetry canonical_types;
    graph_vector_growth_telemetry named_type_refs;

    std::size_t changed_sources = 0;
    std::size_t changed_entities = 0;
    std::size_t changed_types = 0;
    std::size_t validation_visited_types = 0;
    std::size_t validation_visited_type_refs = 0;
    std::size_t validation_dependency_edges = 0;
};

struct graph_storage_snapshot {
    const void* identity_data = nullptr;
    const void* entities_data = nullptr;
    const void* types_data = nullptr;
    const void* member_records_data = nullptr;
    const void* enum_value_records_data = nullptr;
    const void* canonical_types_data = nullptr;
    const void* named_type_refs_data = nullptr;

    std::size_t identity_size = 0;
    std::size_t entities_size = 0;
    std::size_t types_size = 0;
    std::size_t member_records_size = 0;
    std::size_t enum_value_records_size = 0;
    std::size_t canonical_types_size = 0;
    std::size_t named_type_refs_size = 0;

    std::size_t identity_capacity = 0;
    std::size_t entities_capacity = 0;
    std::size_t types_capacity = 0;
    std::size_t member_records_capacity = 0;
    std::size_t enum_value_records_capacity = 0;
    std::size_t canonical_types_capacity = 0;
    std::size_t named_type_refs_capacity = 0;
};

#endif


// Selects the canonical generation algorithm for one build transaction.
// Rebuild constructs G0; incremental applies a sparse project delta Gn -> Gn+1.
enum class graph_build_mode : std::uint8_t {
    rebuild,
    incremental
};

// Classifies the canonical Entity represented by one stable_id.
enum class entity_kind : std::uint8_t {
    aggregate_type,
    enum_type
};

// Classifies the generation-local user-type payload attached to an Entity.
enum class user_type_kind : std::uint8_t {
    aggregate,
    enumeration
};

// Canonical hot entry addressed directly by stable_id in the Project Entity namespace.
// The stable_id is intentionally not duplicated inside the entry; an empty name
// is the single tombstone state for an unoccupied historical identity slot.
struct entity_entry {
    entity_kind kind = entity_kind::aggregate_type;
    string_id name{};
    type_handle type{};

    [[nodiscard]] constexpr bool live() const noexcept { return static_cast<bool>(name); }
};


// Canonical materialized enum value stored by Graph.
struct enum_value_record {
    string_id name{};
    std::uint64_t bits = 0;
};

// Builder input for one enum value before ABI conversion/materialization.
struct enum_value_build {
    string_id name{};
    integral_constant value{};
};

// Builder input for one enum declaration or definition.
struct enum_build_data {
    enum_definition_state definition_state = enum_definition_state::defined;
    bool scoped = false;
    std::optional<builtin_type> explicit_underlying;
    std::span<const enum_value_build> enumerators;
};

// One-based range into the definition arena selected by TypeEntry::kind.
// begin == 0 is the only no-definition state; a defined empty type therefore
// has begin != 0 and count == 0.
struct definition_range {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return begin != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }
};

// Canonical enum declaration semantics that remain meaningful even while the
// enum is opaque. Definition presence is represented only by definition_range.
struct enum_entry {
    bool scoped = false;
    bool fixed_underlying = false;
    builtin_type underlying = builtin_type::integer;
};

// Canonical non-static instance member.
// Member identity is local to the containing aggregate; its type is a canonical
// TypeRef and is interpreted within the same committed Graph generation.
struct member_record {
    string_id name{};
    TypeRef type{};
};

static_assert(sizeof(member_record) == 8);

// Builder input for one canonical TypeRef modifier.
struct type_modifier_build {
    derived_type_kind kind = derived_type_kind::pointer;
    std::uint64_t payload = 0;
};

// Builder input for one aggregate member.
// A named base may remain canonically pending until graph_update::prepare_publish;
// no source-language lookup is performed by Graph.
struct member_build {
    string_id name{};
    std::optional<builtin_type> builtin;
    string_id user_type_name{};
    std::uint32_t modifier_offset = 0;
    std::uint32_t modifier_count = 0;
};

// Generation-local hot Type entry addressed directly by type_handle.
// For aggregate types definition indexes MemberEntry storage; for enum types it
// indexes EnumeratorEntry storage. No committed declared/defined status is kept.
struct type_entry {
    user_type_kind kind = user_type_kind::enumeration;
    enum_entry enumeration{};
    definition_range definition{};
};

// Owns the authoritative canonical compiled state G for one Project generation.
// Graph owns stable Entity identity, generation-local type slots, canonical
// TypeRefs and ABI-dependent canonical materialization. Build-side Source
// contribution bookkeeping is deliberately owned outside G.
// Parser/source-language lookup remains outside this layer.
class graph final {
public:
    graph();
    ~graph();

    graph(const graph&) = delete;
    graph& operator=(const graph&) = delete;
    graph(graph&&) = delete;
    graph& operator=(graph&&) = delete;

    [[nodiscard]] status initialize(abi_configuration abi = {}) noexcept;
    [[nodiscard]] graph_update begin_update(
        graph_build_mode mode,
        source_contribution_cache_update& contributions) noexcept;

    [[nodiscard]] const entity_entry* find(string_id name) const noexcept;
    [[nodiscard]] const entity_entry* find(stable_id id) const noexcept;
    [[nodiscard]] stable_id find_id(string_id name) const noexcept;
    [[nodiscard]] const type_entry* find(type_handle handle) const noexcept;

    [[nodiscard]] std::span<const enum_value_record> enum_values(
        type_handle handle) const noexcept;

    [[nodiscard]] std::span<const member_record> members(
        type_handle handle) const noexcept;

    [[nodiscard]] std::size_t member_count(type_handle handle) const noexcept {
        return members(handle).size();
    }

    [[nodiscard]] const member_record* member(
        type_handle handle,
        member_index index) const noexcept;

    [[nodiscard]] member_index find_member(
        type_handle handle,
        string_id name) const noexcept;

    [[nodiscard]] canonical_type_kind kind(TypeRef type) const noexcept;
    [[nodiscard]] bool builtin(TypeRef type, builtin_type& output) const noexcept;
    [[nodiscard]] bool named(TypeRef type, type_handle& output) const noexcept;
    [[nodiscard]] const derived_type_record* derived(TypeRef type) const noexcept;

    [[nodiscard]] std::size_t derived_type_count() const noexcept;

    [[nodiscard]] std::size_t entity_count() const noexcept {
        return entity_count_value;
    }

    [[nodiscard]] std::size_t user_type_count() const noexcept {
        return user_type_count_value;
    }

    [[nodiscard]] abi_configuration abi() const noexcept {
        return abi_config;
    }

    [[nodiscard]] status export_compiled(
        compiled_graph_state& output) const noexcept;

    [[nodiscard]] status import_compiled(
        const compiled_graph_state& input) noexcept;

    void swap_compiled(graph& other) noexcept;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    [[nodiscard]] graph_storage_snapshot storage_snapshot_for_testing() const noexcept;
#endif

private:
    struct type_storage;
    struct type_build_state;
    struct entity_slot;
    struct candidate_identity_slot;
    struct candidate_entity_slot;
    struct candidate_type_slot;

    // One canonical TypeRef table entry. Builtins are intrinsic; named entries
    // refer to generation-local type slots; derived entries wrap another TypeRef.
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

        friend bool operator==(
            const derived_type_key&,
            const derived_type_key&) noexcept = default;
    };

    struct derived_type_key_hash {
        [[nodiscard]] std::size_t operator()(
            const derived_type_key& key) const noexcept;
    };

    [[nodiscard]] status rebuild_dependency_index() noexcept;

    friend class graph_update;
    friend class graph_build_transaction_test_access;

    // type_handle is one-based; slot N addresses types[N - 1].
    std::vector<std::unique_ptr<type_storage>> types;
    std::vector<std::uint32_t> free_type_slots;

    // stable_id values directly index entities; slot zero is not a live Entity.
    std::vector<entity_slot> entities;

    // Dense string_id -> stable_id identity index.
    std::vector<stable_id> identity;

    // Append-only definition arenas. TypeEntry::definition stores one-based
    // ranges into exactly one arena selected by TypeEntry::kind.
    std::vector<member_record> member_records;
    std::vector<enum_value_record> enum_value_records;

    // Canonical TypeRef table; index zero is invalid/sentinel.
    std::vector<canonical_type_record> canonical_types;
    std::vector<TypeRef> named_type_refs;
    std::unordered_map<derived_type_key, TypeRef, derived_type_key_hash>
        derived_type_index;

    // Reconstructable acceleration indexes used only to bound incremental
    // validation to the changed type closure.
    std::vector<std::vector<std::uint32_t>> type_dependencies;
    std::vector<std::vector<std::uint32_t>> reverse_type_dependents;

    std::size_t entity_count_value = 0;
    std::size_t user_type_count_value = 0;
    std::uint32_t next_stable_id = 1;
    // Generation is relative to the latest explicit Build/Rebuild: G0 is zero,
    // each incremental commit advances Gn -> Gn+1.
    std::uint64_t generation = 0;
    abi_configuration abi_config{};

    // Reusable generation-tagged candidate overlays avoid cloning the full Graph
    // for each update. Only touched slots are materialized in the active candidate.
    std::vector<candidate_identity_slot> candidate_identities;
    std::vector<candidate_entity_slot> candidate_entities;
    std::vector<candidate_type_slot> candidate_types;
    std::uint64_t next_candidate_generation = 1;
};

// Represents one isolated candidate mutation of Graph.
// graph_update overlays only touched identity/entity/type slots, validates
// against Source Manager and String Registry candidates, then publishes prepared
// state without allocation-sensitive work.
class graph_update final {
public:
    // Restricts one Source replacement to declarations/types contributed by that
    // Source while delegating canonical identity and TypeRef creation to graph_update.
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
            std::span<const member_build> members,
            std::span<const type_modifier_build> modifiers) noexcept;

        [[nodiscard]] status resolve_type(
            string_id name,
            TypeRef& output) const noexcept;

        [[nodiscard]] status get_or_create_pointer(
            TypeRef child,
            TypeRef& output) noexcept;

        [[nodiscard]] status get_or_create_array(
            TypeRef child,
            std::uint64_t extent,
            TypeRef& output) noexcept;

        [[nodiscard]] status get_or_create_lvalue_reference(
            TypeRef child,
            TypeRef& output) noexcept;

        [[nodiscard]] status get_or_create_rvalue_reference(
            TypeRef child,
            TypeRef& output) noexcept;

        [[nodiscard]] TypeRef builtin_type_ref(
            builtin_type value) const noexcept;

    private:
        friend class graph_update;

        source_replacement(
            graph_update& owner_update,
            source_id owner_source) noexcept
            : update(&owner_update), source(owner_source) {}

        graph_update* update = nullptr;
        source_id source{};
    };

    ~graph_update();

    graph_update(const graph_update&) = delete;
    graph_update& operator=(const graph_update&) = delete;

    graph_update(graph_update&& other) noexcept;
    graph_update& operator=(graph_update&&) = delete;

    [[nodiscard]] status replace_source(
        source_id source,
        source_replacement& replacement) noexcept;

    [[nodiscard]] const entity_entry* find(string_id name) const noexcept;
    [[nodiscard]] const entity_entry* find(stable_id id) const noexcept;
    [[nodiscard]] stable_id find_id(string_id name) const noexcept;
    [[nodiscard]] const type_entry* find(type_handle handle) const noexcept;

    [[nodiscard]] std::span<const enum_value_record> enum_values(
        type_handle handle) const noexcept;

private:
    friend class graph;
    friend class graph_build_transaction;
    friend class graph_build_transaction_test_access;

    graph_update(
        graph& owner,
        source_contribution_cache_update& contributions,
        std::uint64_t generation,
        std::uint64_t candidate_generation,
        bool full_reconstruction) noexcept;

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

    [[nodiscard]] status begin_source_replacement(source_id source) noexcept;
    [[nodiscard]] status remove_named_entity_for_testing(stable_id id) noexcept;

    [[nodiscard]] status build_contribution(
        const enum_build_data& data,
        source_contribution_record& output) noexcept;

    [[nodiscard]] status add_delta(
        source_id source,
        stable_id id,
        const source_contribution_record& contribution) noexcept;

    [[nodiscard]] status remove_delta(
        source_id source,
        const source_contribution_record& contribution) noexcept;

    [[nodiscard]] status materialize(
        stable_id id,
        string_id name) noexcept;

    [[nodiscard]] status assign_type(
        stable_id id,
        graph::entity_slot& entity,
        std::unique_ptr<graph::type_storage> type) noexcept;

    [[nodiscard]] status get_or_create_named_type_ref(
        type_handle handle,
        TypeRef& output) noexcept;

    [[nodiscard]] status get_or_create_derived(
        derived_type_kind kind,
        TypeRef child,
        std::uint64_t payload,
        TypeRef& output) noexcept;

    [[nodiscard]] status resolve_pending_members(
        graph::type_build_state& build) noexcept;

    // Validates only the transitive reverse dependency closure rooted at
    // changed type slots for incremental builds; G0 validates every live type.
    [[nodiscard]] status validate_live_member_type_refs() noexcept;

    [[nodiscard]] status prepare_dependency_index_updates() noexcept;
    [[nodiscard]] status build_rebuild_dependency_index() noexcept;
    void rollback_prepared_owner_growth() noexcept;

    [[nodiscard]] status canonicalize_new_stable_ids(
        const string_registry_update& strings) noexcept;

    [[nodiscard]] status prepare_full_reconstruction() noexcept;
    [[nodiscard]] status rebuild_canonical_type_table() noexcept;
    [[nodiscard]] status build_rebuild_storage() noexcept;

    // Marks the string_id slots that must survive G0 physical String Registry
    // reclamation. Numeric string IDs are never remapped.
    [[nodiscard]] status collect_rebuild_string_retention(
        std::size_t candidate_string_slots,
        std::vector<std::uint8_t>& retained) const noexcept;

    graph::candidate_identity_slot& touch_identity(std::uint32_t name);
    graph::candidate_entity_slot& touch_entity(std::uint32_t id);
    graph::candidate_type_slot& touch_type(std::uint32_t handle);

    graph* owner = nullptr;
    source_contribution_cache_update* contributions = nullptr;

    // Retained for the current update contract even though current materialization
    // paths use candidate_type_slot ownership directly.
    std::vector<std::unique_ptr<graph::type_storage>> owned_types;

    std::vector<std::uint32_t> changed_identities;
    std::vector<std::uint32_t> changed_entities;
    std::vector<std::uint32_t> changed_types;
    std::vector<std::uint32_t> changed_sources;

    std::vector<std::uint32_t> claimed_free_type_slots;

    // Rebuild-only complete canonical storage. G0 is materialized into detached
    // arrays and publication swaps them into Graph in one operation; sparse
    // changed_* overlays remain an incremental Gn -> Gn+1 implementation detail.
    std::vector<stable_id> rebuilt_identity;
    std::vector<graph::entity_slot> rebuilt_entities;
    std::vector<std::unique_ptr<graph::type_storage>> rebuilt_types;
    std::vector<std::uint32_t> rebuilt_free_type_slots;
    std::size_t rebuilt_entity_count = 0;
    std::size_t rebuilt_type_count = 0;

    // Rebuild-only compact definition arenas. A G0 publication swaps these
    // fresh arenas into Graph so obsolete incremental definition slices are
    // reclaimed without remapping TypeEntry ranges during Gn -> Gn+1 updates.
    std::vector<member_record> rebuilt_member_records;
    std::vector<enum_value_record> rebuilt_enum_value_records;

    // Rebuild-only canonical TypeRef state. TypeRef is generation-local, so G0
    // may compact/reindex the table without affecting persistent Entity identity.
    // Incremental Gn -> Gn+1 remains append-only and preserves every existing ref.
    std::vector<graph::canonical_type_record> rebuilt_canonical_types;
    std::vector<TypeRef> rebuilt_named_type_refs;
    std::unordered_map<
        graph::derived_type_key,
        TypeRef,
        graph::derived_type_key_hash> rebuilt_derived_type_index;

    std::vector<std::vector<std::uint32_t>> rebuilt_type_dependencies;
    std::vector<std::vector<std::uint32_t>> rebuilt_reverse_type_dependents;

    std::vector<graph::canonical_type_record> added_canonical_types;
    std::vector<std::pair<std::uint32_t, TypeRef>> added_named_type_refs;
    std::unordered_map<std::uint32_t, TypeRef> added_named_type_index;
    std::unordered_map<
        graph::derived_type_key,
        TypeRef,
        graph::derived_type_key_hash> added_derived_type_index;

    struct dependency_list_update {
        std::uint32_t handle = 0;
        std::vector<std::uint32_t> values;
    };

    std::vector<dependency_list_update> prepared_type_dependency_updates;
    std::vector<dependency_list_update> prepared_reverse_dependency_updates;

    std::size_t prepared_identity_size = 0;
    std::size_t prepared_entities_size = 0;
    std::size_t prepared_types_size = 0;
    std::size_t prepared_named_type_refs_size = 0;
    std::size_t prepared_type_dependencies_size = 0;
    std::size_t prepared_reverse_type_dependents_size = 0;
    bool prepared_owner_growth = false;

    std::uint32_t next_stable_id = 1;
    std::uint32_t next_type_slot = 1;
    std::uint64_t base_generation = 0;
    std::uint64_t candidate_generation = 0;
    bool full_reconstruction = false;

    status failure{};
    bool prepared = false;
    bool committed = false;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    graph_storage_prepare_telemetry storage_telemetry;
#endif
};

} // namespace cw::server
