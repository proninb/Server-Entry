#pragma once

#include "../../member_index.hpp"
#include "../../source_id.hpp"
#include "../../type_id.hpp"
#include "../../status.hpp"
#include "../../string_id.hpp"
#include "../../type_handle.hpp"
#include "../source_entity_ref.hpp"
#include "../language/aggregate_semantics.hpp"
#include "../language/enum_semantics.hpp"
#include "builtin_type.hpp"
#include "enum_build.hpp"
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

// Samples detailed named-enum Graph mutation work without perturbing every
// canonical operation. Every 1024th named enum is measured; all other calls
// execute without clock reads.
struct graph_named_enum_telemetry {
    static constexpr std::uint64_t sample_stride = 1024;

    std::uint64_t calls = 0;
    std::uint64_t samples = 0;

    std::uint64_t total_ns = 0;

    std::uint64_t identity_ns = 0;
    std::uint64_t contribution_build_ns = 0;
    std::uint64_t reconcile_ns = 0;
    std::uint64_t delta_ns = 0;
    std::uint64_t contribution_append_ns = 0;
    std::uint64_t materialize_ns = 0;

    std::uint64_t materialize_state_touch_ns = 0;
    std::uint64_t materialize_type_storage_ns = 0;
    std::uint64_t materialize_build_state_ns = 0;
    std::uint64_t materialize_assign_type_ns = 0;

    std::uint64_t assign_type_handle_ns = 0;
    std::uint64_t assign_type_touch_type_ns = 0;
    std::uint64_t assign_type_candidate_store_ns = 0;
    std::uint64_t assign_type_named_type_ref_ns = 0;

    std::uint64_t named_type_ref_existing_lookup_ns = 0;
    std::uint64_t named_type_ref_canonical_append_ns = 0;
    std::uint64_t named_type_ref_mapping_append_ns = 0;
    std::uint64_t named_type_ref_index_emplace_ns = 0;

    std::uint64_t materialize_attach_ns = 0;



    [[nodiscard]] bool begin_call() noexcept {
        ++calls;

        if ((calls % sample_stride) != 0) {
            return false;
        }

        ++samples;
        return true;
    }
};

struct graph_prepare_phase_telemetry {
    std::uint64_t pending_member_resolution_ns = 0;
    std::uint64_t live_typeref_validation_ns = 0;
    std::uint64_t canonical_typeref_rebuild_ns = 0;
    std::uint64_t string_validation_ns = 0;
    std::uint64_t definition_scan_ns = 0;
    std::uint64_t definition_materialization_ns = 0;
    std::uint64_t rebuild_storage_ns = 0;
    std::uint64_t dependency_index_ns = 0;
    std::uint64_t final_prepare_ns = 0;
};
class source_manager_update;
class source_contribution_cache;
class source_contribution_cache_update;
struct source_contribution_record;
struct source_contribution_state;
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

    std::size_t pending_resolution_types = 0;
    std::size_t pending_resolution_members = 0;
    std::size_t pending_resolution_modifiers = 0;

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

// Classifies the canonical Entity represented by one type_id.
enum class entity_kind : std::uint8_t {
    aggregate_type,
    enum_type
};

// Classifies the generation-local user-type payload attached to an Entity.
enum class user_type_kind : std::uint8_t {
    aggregate,
    enumeration
};

// Canonical hot entry addressed directly by type_id in the Project Entity namespace.
// The type_id is intentionally not duplicated inside the entry; an empty name
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

// Cold ABI layout for one committed user type. A zero size/alignment pair is
// the only unavailable-layout state and is valid for incomplete aggregates.
struct type_layout_record {
    std::uint64_t size = 0;
    std::uint32_t alignment = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return size != 0 && alignment != 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
};

static_assert(sizeof(type_layout_record) == 16);

// Cold byte offset parallel to member_record. Offsets are 64-bit because array
// extents and aggregate sizes are already represented with 64-bit quantities.
struct member_layout_record {
    std::uint64_t offset = 0;
};

static_assert(sizeof(member_layout_record) == 8);

// Canonical source-local static storage used by declarative construction.
// It has no Project-global type_id; the one-based arena coordinate is valid
// only inside this committed Graph generation.
struct static_object_record {
    TypeRef type{};
};

static_assert(sizeof(static_object_record) == 4);

// One default binding for a non-static lvalue-reference member.
struct construction_binding_record {
    member_index member{};
    std::uint32_t static_object = 0;
};

static_assert(sizeof(construction_binding_record) == 8);

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

    // Compatibility canonical-name path for non-Parser producers.
    string_id user_type_name{};

    std::uint32_t modifier_offset = 0;
    std::uint32_t modifier_count = 0;

    // Production Parser relation; resolved without text/string lookup.
    source_entity_ref user_type_entity{};
};

// Generation-local hot Type entry addressed directly by type_handle.
// For aggregate types definition indexes MemberEntry storage; for enum types it
// indexes EnumeratorEntry storage. No committed declared/defined status is kept.
struct type_entry {
    user_type_kind kind = user_type_kind::enumeration;
    enum_entry enumeration{};
    definition_range definition{};
};


class graph;

// Read-only capability over one complete canonical Type domain.
// A candidate view is valid only after graph_update::seal_types() and before
// final transaction preparation/publication. It never exposes Graph mutation.
class graph_type_view final {
public:
    graph_type_view() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] const entity_entry* find(string_id name) const noexcept;
    [[nodiscard]] const entity_entry* find(type_id id) const noexcept;
    [[nodiscard]] type_id find_id(string_id name) const noexcept;
    [[nodiscard]] const type_entry* find(type_handle handle) const noexcept;

    [[nodiscard]] std::span<const enum_value_record> enum_values(
        type_handle handle) const noexcept;

    [[nodiscard]] std::span<const member_record> members(
        type_handle handle) const noexcept;

    [[nodiscard]] const member_record* member(
        type_handle handle,
        member_index index) const noexcept;

    [[nodiscard]] member_index find_member(
        type_handle handle,
        string_id name) const noexcept;

    [[nodiscard]] TypeRef type_ref(type_handle handle) const noexcept;
    [[nodiscard]] TypeRef type_ref(builtin_type type) const noexcept;

    [[nodiscard]] bool builtin(
        TypeRef type,
        builtin_type& output) const noexcept;

    [[nodiscard]] bool named(
        TypeRef type,
        type_handle& output) const noexcept;

    [[nodiscard]] const derived_type_record* derived(
        TypeRef type) const noexcept;

    [[nodiscard]] const type_layout_record* layout(
        type_handle handle) const noexcept;

    [[nodiscard]] const member_layout_record* member_layout(
        type_handle handle,
        member_index index) const noexcept;

private:
    friend class graph;
    friend class graph_update;

    explicit graph_type_view(const graph& value) noexcept
        : committed(&value) {}

    explicit graph_type_view(const graph_update& value) noexcept
        : candidate(&value) {}

    [[nodiscard]] bool candidate_ready() const noexcept;

    const graph* committed = nullptr;
    const graph_update* candidate = nullptr;
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

    [[nodiscard]] graph_type_view type_view() const noexcept {
        return graph_type_view{*this};
    }

    [[nodiscard]] const entity_entry* find(string_id name) const noexcept;
    [[nodiscard]] const entity_entry* find(type_id id) const noexcept;
    [[nodiscard]] type_id find_id(string_id name) const noexcept;
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

    [[nodiscard]] std::span<const static_object_record>
        static_objects() const noexcept {
        return static_object_records;
    }

    [[nodiscard]] const static_object_record* static_object(
        std::uint32_t index) const noexcept;

    [[nodiscard]] std::span<const construction_binding_record>
        construction_bindings(type_handle handle) const noexcept;

    // Returns the canonical generation-local TypeRef for an already committed
    // type. These are read-only semantic queries for post-G0 construction.
    [[nodiscard]] TypeRef type_ref(type_handle handle) const noexcept;
    [[nodiscard]] TypeRef type_ref(builtin_type type) const noexcept;

    [[nodiscard]] const type_layout_record* layout(
        type_handle handle) const noexcept;

    [[nodiscard]] bool layout(
        TypeRef type,
        type_layout_record& output) const noexcept;

    [[nodiscard]] const member_layout_record* member_layout(
        type_handle handle,
        member_index index) const noexcept;

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
    // Dense type_handle slot. Pointers returned by find(type_handle) are views
    // into one committed Graph generation and do not survive publication.
    struct type_storage {
        type_entry record{};
    };

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
    [[nodiscard]] status rebuild_layout_sidecars() noexcept;

    friend class graph_update;
    friend class graph_type_view;
    friend class graph_build_transaction_test_access;

    // type_handle is one-based; slot N addresses types[N - 1]. Incremental
    // updates append new slots; removed slots stay empty until Rebuild/G0.
    // Thus old named/derived TypeRefs cannot acquire a different meaning.
    std::vector<std::optional<type_storage>> types;

    // type_id values directly index entities; slot zero is not a live Entity.
    std::vector<entity_slot> entities;

    // Dense string_id -> type_id identity index.
    std::vector<type_id> identity;

    // Append-only definition arenas. TypeEntry::definition stores one-based
    // ranges into exactly one arena selected by TypeEntry::kind.
    std::vector<member_record> member_records;
    std::vector<enum_value_record> enum_value_records;

    // Reconstructable ABI layout sidecars. Type layout is indexed directly by
    // type_handle - 1; member layout is parallel to the append-only member arena.
    std::vector<type_layout_record> type_layout_records;
    std::vector<member_layout_record> member_layout_records;

    // Cold construction semantics kept outside hot Entity/Type records.
    std::vector<static_object_record> static_object_records;
    std::vector<construction_binding_record> construction_binding_records;
    std::vector<definition_range> construction_binding_ranges;

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
    std::uint32_t next_type_id = 1;
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

        // Reserves the complete Source-local contribution once before its
        // deterministic per-fact publication loop.
        [[nodiscard]] status reserve(
            std::size_t named_count,
            std::size_t anonymous_count,
            std::size_t enum_value_count) noexcept;
        [[nodiscard]] status add_named_enum(
            string_id name,
            const enum_build_data& data,
            type_id& entity,
            type_handle& type) noexcept;

        [[nodiscard]] status add_named_enum(
            string_id name,
            const enum_build_data& data,
            type_id& entity,
            type_handle& type,
            graph_named_enum_telemetry* telemetry) noexcept;

        [[nodiscard]] status add_anonymous_enum(
            const enum_build_data& data,
            type_handle& type) noexcept;

        [[nodiscard]] status add_named_type(
            string_id name,
            aggregate_definition_state state,
            type_id& entity,
            type_handle& type) noexcept;

        [[nodiscard]] status define_members(
            type_handle type,
            std::span<const member_build> members,
            std::span<const type_modifier_build> modifiers) noexcept;

        [[nodiscard]] status bind_source_entity(
            source_entity_ref reference,
            type_id entity) noexcept;

        [[nodiscard]] status resolve_type(
            source_entity_ref reference,
            TypeRef& output) const noexcept;

        // Compatibility canonical-name path for non-Parser producers.
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

        [[nodiscard]] status add_static_object(
            std::optional<builtin_type> builtin,
            source_entity_ref user_type_entity,
            std::span<const type_modifier_build> modifiers,
            std::uint32_t& object) noexcept;

        [[nodiscard]] status add_construction_binding(
            source_entity_ref owner_type,
            member_index member,
            std::uint32_t static_object) noexcept;

    private:
        friend class graph_update;

        source_replacement(
            graph_update& owner_update,
            source_id owner_source,
            source_contribution_state& owner_state) noexcept
            : update(&owner_update),
              source(owner_source),
              state(&owner_state) {}

        graph_update* update = nullptr;
        source_id source{};
        source_contribution_state* state = nullptr;
    };

    ~graph_update();

    graph_update(const graph_update&) = delete;
    graph_update& operator=(const graph_update&) = delete;

    graph_update(graph_update&& other) noexcept;
    graph_update& operator=(graph_update&&) = delete;

    // Pre-sizes every dense G0 candidate overlay before canonical mutation.
    [[nodiscard]] status reserve_rebuild(
        std::size_t source_slots,
        std::size_t name_slots,
        std::size_t entity_count,
        std::size_t type_count) noexcept;
    [[nodiscard]] status replace_source(
        source_id source,
        source_replacement& replacement) noexcept;

    // Finalizes the complete G0 Type domain without publishing Graph.
    // After success all Type identities, TypeRefs, members and ABI layout
    // are immutable and readable only through graph_type_view.
    [[nodiscard]] status seal_types(
        const source_manager_update& sources,
        const string_registry_update& strings) noexcept;

    [[nodiscard]] graph_type_view type_view() const noexcept {
        return types_sealed && full_reconstruction &&
               !prepared && !committed
            ? graph_type_view{*this}
            : graph_type_view{};
    }

    [[nodiscard]] const entity_entry* find(string_id name) noexcept;
    [[nodiscard]] const entity_entry* find(type_id id) noexcept;
    [[nodiscard]] type_id find_id(string_id name) const noexcept;
    [[nodiscard]] const type_entry* find(type_handle handle) noexcept;

    [[nodiscard]] std::span<const enum_value_record> enum_values(
        type_handle handle) noexcept;

    [[nodiscard]] const graph_prepare_phase_telemetry&
        prepare_phase_telemetry() const noexcept {
        return prepare_telemetry;
    }

private:
    friend class graph;
    friend class graph_type_view;
    friend class graph_build_transaction;
    friend class graph_build_transaction_test_access;

    graph_update(
        graph& owner,
        source_contribution_cache_update& contributions,
        std::uint64_t generation,
        std::uint64_t candidate_generation,
        bool full_reconstruction) noexcept;

    // Mutates the Source candidate already opened by replace_source().
    // Only source_replacement may enter these per-fact paths.
    [[nodiscard]] status add_named_enum_from_replacement(
        string_id name,
        source_id source,
        source_contribution_state& source_state,
        const enum_build_data& data,
        type_id& entity,
        type_handle& type,
        graph_named_enum_telemetry* telemetry = nullptr) noexcept;

    [[nodiscard]] status add_named_type_from_replacement(
        string_id name,
        source_id source,
        source_contribution_state& source_state,
        aggregate_definition_state state,
        type_id& entity,
        type_handle& type) noexcept;

    [[nodiscard]] status add_anonymous_enum_from_replacement(
        source_id source,
        source_contribution_state& source_state,
        const enum_build_data& data,
        type_handle& type) noexcept;

    [[nodiscard]] status resolve_source_type(
        source_entity_ref reference,
        TypeRef& output) noexcept;

    [[nodiscard]] status prepare_publish(
        const source_manager_update& sources,
        const string_registry_update& strings) noexcept;

    [[nodiscard]] status prepare_sealed_rebuild_publish() noexcept;

    graph_prepare_phase_telemetry prepare_telemetry{};

    void publish_prepared() noexcept;
    void cancel() noexcept;

    [[nodiscard]] status begin_source_replacement(
        source_id source,
        source_contribution_state*& state) noexcept;

    [[nodiscard]] status flush_retained_source_replacement(
        source_id source) noexcept;

    [[nodiscard]] status flush_retained_source_replacements() noexcept;

    [[nodiscard]] status reconcile_retained_enum(
        source_id source,
        source_contribution_state& source_state,
        const source_contribution_record& contribution,
        bool& reconciled) noexcept;

    [[nodiscard]] status remove_named_entity_for_testing(type_id id) noexcept;

    [[nodiscard]] status build_contribution(
        source_contribution_state& state,
        const enum_build_data& data,
        source_contribution_record& output) noexcept;

    [[nodiscard]] status add_delta(
        source_id source,
        type_id id,
        const source_contribution_record& contribution) noexcept;

    [[nodiscard]] status remove_delta(
        source_id source,
        const source_contribution_record& contribution) noexcept;

    [[nodiscard]] status materialize(
        type_id id,
        string_id name) noexcept;

    [[nodiscard]] status materialize(
        type_id id,
        string_id name,
        type_handle& type) noexcept;

    [[nodiscard]] status materialize_sampled(
        type_id id,
        string_id name,
        type_handle& type,
        graph_named_enum_telemetry& telemetry) noexcept;

    template <bool Detailed>
    [[nodiscard]] status materialize_impl(
        type_id id,
        string_id name,
        type_handle* type,
        graph_named_enum_telemetry* telemetry) noexcept;

    [[nodiscard]] status assign_type(
        type_id id,
        graph::entity_slot& entity,
        graph::type_storage type) noexcept;

    [[nodiscard]] status assign_type_sampled(
        type_id id,
        graph::entity_slot& entity,
        graph::type_storage type,
        graph_named_enum_telemetry& telemetry) noexcept;

    template <bool Detailed>
    [[nodiscard]] status assign_type_impl(
        type_id id,
        graph::entity_slot& entity,
        graph::type_storage type,
        graph_named_enum_telemetry* telemetry) noexcept;

    [[nodiscard]] status get_or_create_named_type_ref(
        type_handle handle,
        TypeRef& output) noexcept;

    [[nodiscard]] status get_or_create_named_type_ref_sampled(
        type_handle handle,
        TypeRef& output,
        graph_named_enum_telemetry& telemetry) noexcept;

    template <bool Detailed>
    [[nodiscard]] status get_or_create_named_type_ref_impl(
        type_handle handle,
        TypeRef& output,
        graph_named_enum_telemetry* telemetry) noexcept;

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


    [[nodiscard]] status prepare_full_reconstruction() noexcept;
    [[nodiscard]] status rebuild_canonical_type_table() noexcept;
    [[nodiscard]] status build_rebuild_storage() noexcept;
    [[nodiscard]] status build_rebuild_layout() noexcept;
    [[nodiscard]] status prepare_incremental_layout_updates() noexcept;
    [[nodiscard]] status resolve_pending_construction() noexcept;
    [[nodiscard]] status build_rebuild_construction() noexcept;

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


    std::vector<std::uint32_t> changed_identities;
    std::vector<std::uint32_t> changed_entities;
    std::vector<std::uint32_t> changed_types;
    std::vector<std::uint32_t> changed_sources;


    // Rebuild-only complete canonical storage. G0 is materialized into detached
    // arrays and publication swaps them into Graph in one operation; sparse
    // changed_* overlays remain an incremental Gn -> Gn+1 implementation detail.
    std::vector<type_id> rebuilt_identity;
    std::vector<graph::entity_slot> rebuilt_entities;
    std::vector<std::optional<graph::type_storage>> rebuilt_types;
    std::size_t rebuilt_entity_count = 0;
    std::size_t rebuilt_type_count = 0;

    // Rebuild-only compact definition arenas. A G0 publication swaps these
    // fresh arenas into Graph so obsolete incremental definition slices are
    // reclaimed without remapping TypeEntry ranges during Gn -> Gn+1 updates.
    std::vector<member_record> rebuilt_member_records;
    std::vector<enum_value_record> rebuilt_enum_value_records;
    std::vector<type_layout_record> rebuilt_type_layout_records;
    std::vector<member_layout_record> rebuilt_member_layout_records;

    struct pending_static_object {
        std::optional<builtin_type> builtin;
        source_entity_ref user_type_entity{};
        std::uint32_t modifier_offset = 0;
        std::uint32_t modifier_count = 0;
    };

    struct pending_construction_binding {
        source_entity_ref owner_type{};
        member_index member{};
        std::uint32_t static_object = 0;
        TypeRef resolved_owner{};
    };

    std::vector<pending_static_object> pending_static_objects;
    std::vector<type_modifier_build> pending_static_modifiers;
    std::vector<static_object_record> rebuilt_static_object_records;
    std::vector<pending_construction_binding> pending_construction_bindings;
    std::vector<construction_binding_record>
        rebuilt_construction_binding_records;
    std::vector<definition_range>
        rebuilt_construction_binding_ranges;

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

    struct prepared_type_layout_update {
        std::uint32_t handle = 0;
        std::uint8_t state = 0;
        type_layout_record layout{};
        std::vector<member_layout_record> members;
    };

    std::vector<prepared_type_layout_update>
        prepared_type_layout_updates;

    struct dependency_list_update {
        std::uint32_t handle = 0;
        std::vector<std::uint32_t> values;
    };

    std::vector<dependency_list_update> prepared_type_dependency_updates;
    std::vector<dependency_list_update> prepared_reverse_dependency_updates;

    std::size_t prepared_identity_size = 0;
    std::size_t prepared_entities_size = 0;
    std::size_t prepared_types_size = 0;
    std::size_t prepared_type_layout_size = 0;
    std::size_t prepared_named_type_refs_size = 0;
    std::size_t prepared_type_dependencies_size = 0;
    std::size_t prepared_reverse_type_dependents_size = 0;
    bool prepared_owner_growth = false;

    std::uint32_t next_type_id = 1;
    std::uint32_t next_type_slot = 1;
    std::uint64_t base_generation = 0;
    std::uint64_t candidate_generation = 0;
    bool full_reconstruction = false;

    // True only after every currently opened Source replacement has had any
    // retained previous contribution reconciled or removed. A new Source
    // replacement invalidates this transaction-local readiness state.
    bool retained_replacements_flushed = false;

    status failure{};
    bool sealing_types = false;
    bool types_sealed = false;
    bool prepared = false;
    bool committed = false;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
    graph_storage_prepare_telemetry storage_telemetry;
#endif
};

} // namespace cw::server
