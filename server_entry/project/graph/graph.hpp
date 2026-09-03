#pragma once
#include "../../source_id.hpp"
#include "../../stable_id.hpp"
#include "../../status.hpp"
#include "../../string_id.hpp"
#include "../../type_handle.hpp"
#include "../../member_index.hpp"
#include "builtin_type.hpp"
#include "type_ref.hpp"
#include "../language/enum_semantics.hpp"
#include "../language/aggregate_semantics.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace cw::server
{
class source_manager_update; class string_registry_update; class graph_build_transaction;
struct compiled_graph_state;
class graph_build_transaction_test_access; class graph_update;
enum class entity_kind : std::uint8_t { aggregate_type, enum_type };
enum class user_type_kind : std::uint8_t { aggregate, enumeration };
struct entity_record { stable_id id{}; entity_kind kind=entity_kind::enum_type; string_id name{}; source_id defining_source{}; type_handle type{}; };
struct enum_value_record { string_id name{}; std::uint64_t bits=0; };
struct enum_value_build { string_id name{}; integral_constant value{}; };
struct enum_build_data { enum_definition_state definition_state=enum_definition_state::defined; bool scoped=false; std::optional<builtin_type> explicit_underlying; std::span<const enum_value_build> enumerators; };
struct enum_type_record { enum_definition_state definition_state=enum_definition_state::opaque; bool scoped=false; bool fixed_underlying=false; builtin_type underlying=builtin_type::integer; std::uint32_t enumerator_count=0; };
struct aggregate_type_record { aggregate_definition_state definition_state=aggregate_definition_state::declared; };
struct member_record { string_id name{}; TypeRef type{}; };
static_assert(sizeof(member_record)==8);
struct member_build { string_id name{}; TypeRef type{}; };
struct user_type_record { user_type_kind kind=user_type_kind::enumeration; enum_type_record enumeration{}; aggregate_type_record aggregate{}; };

class graph final
{
public:
    graph(); ~graph();
    graph(const graph&)=delete; graph& operator=(const graph&)=delete;
    graph(graph&&)=delete; graph& operator=(graph&&)=delete;
    [[nodiscard]] status initialize(abi_configuration abi={}) noexcept;
    [[nodiscard]] graph_update begin_update() noexcept;
    [[nodiscard]] const entity_record* find(string_id name) const noexcept;
    [[nodiscard]] const entity_record* find(stable_id id) const noexcept;
    [[nodiscard]] const user_type_record* find(type_handle handle) const noexcept;
    [[nodiscard]] std::span<const enum_value_record> enum_values(type_handle handle) const noexcept;
    [[nodiscard]] std::span<const member_record> members(type_handle handle) const noexcept;
    [[nodiscard]] std::size_t member_count(type_handle handle) const noexcept { return members(handle).size(); }
    [[nodiscard]] const member_record* member(type_handle,member_index) const noexcept;
    [[nodiscard]] member_index find_member(type_handle,string_id) const noexcept;
    [[nodiscard]] canonical_type_kind kind(TypeRef) const noexcept;
    [[nodiscard]] bool builtin(TypeRef,builtin_type&) const noexcept;
    [[nodiscard]] bool named(TypeRef,type_handle&) const noexcept;
    [[nodiscard]] const derived_type_record* derived(TypeRef) const noexcept;
    [[nodiscard]] std::size_t derived_type_count() const noexcept;
    [[nodiscard]] std::size_t contribution_count(source_id source) const noexcept;
    [[nodiscard]] std::size_t entity_count() const noexcept { return entity_count_; }
    [[nodiscard]] std::size_t user_type_count() const noexcept { return user_type_count_; }
    [[nodiscard]] abi_configuration abi() const noexcept { return abi_; }
    [[nodiscard]] status export_compiled(compiled_graph_state& output) const noexcept;
    [[nodiscard]] status import_compiled(const compiled_graph_state& input) noexcept;
    void swap_compiled(graph& other) noexcept;
private:
    struct type_storage; struct definition_payload; struct source_contribution;
    struct source_state; struct enum_aggregate; struct entity_slot;
    struct candidate_identity_slot; struct candidate_entity_slot;
    struct candidate_type_slot; struct candidate_source_slot;
    struct canonical_type_record { canonical_type_kind kind=canonical_type_kind::builtin; builtin_type builtin=builtin_type::void_type; type_handle named{}; derived_type_record derived{}; };
    struct derived_type_key { derived_type_kind kind=derived_type_kind::pointer; TypeRef child{}; std::uint64_t payload=0; friend bool operator==(const derived_type_key&,const derived_type_key&)noexcept=default; };
    struct derived_type_key_hash { std::size_t operator()(const derived_type_key&)const noexcept; };
    friend class graph_update; friend class graph_build_transaction_test_access;
    std::vector<std::unique_ptr<type_storage>> types_;
    std::vector<std::uint32_t> free_type_slots_;
    std::vector<entity_slot> entities_;
    std::vector<stable_id> identity_;
    std::vector<enum_aggregate> enum_aggregates_;
    std::vector<source_state> source_states_;
    std::vector<member_record> members_;
    std::vector<canonical_type_record> canonical_types_;
    std::vector<TypeRef> named_type_refs_;
    std::unordered_map<derived_type_key,TypeRef,derived_type_key_hash> derived_type_index_;
    std::size_t entity_count_=0;
    std::size_t user_type_count_=0;
    std::uint32_t next_stable_id_=1;
    std::uint64_t generation_=0;
    abi_configuration abi_{};
    std::vector<candidate_identity_slot> candidate_identities_;
    std::vector<candidate_entity_slot> candidate_entities_;
    std::vector<candidate_type_slot> candidate_types_;
    std::vector<candidate_source_slot> candidate_sources_;
    std::uint64_t next_candidate_generation_=1;
};

class graph_update final
{
public:
    class source_replacement final
    {
    public:
        source_replacement() noexcept = default;
        source_replacement(const source_replacement&)=delete; source_replacement& operator=(const source_replacement&)=delete;
        source_replacement(source_replacement&&) noexcept=default; source_replacement& operator=(source_replacement&&)=delete;
        [[nodiscard]] status add_named_enum(string_id name,const enum_build_data& data,stable_id& entity,type_handle& type) noexcept;
        [[nodiscard]] status add_anonymous_enum(const enum_build_data& data,type_handle& type) noexcept;
        [[nodiscard]] status add_named_type(string_id name,aggregate_definition_state state,
                                            stable_id& entity,type_handle& type) noexcept;
        [[nodiscard]] status define_members(type_handle,std::span<const member_build>) noexcept;
        [[nodiscard]] status resolve_type(string_id,TypeRef&) const noexcept;
        [[nodiscard]] status get_or_create_pointer(TypeRef,TypeRef&) noexcept;
        [[nodiscard]] status get_or_create_array(TypeRef,std::uint64_t,TypeRef&) noexcept;
        [[nodiscard]] status get_or_create_lvalue_reference(TypeRef,TypeRef&) noexcept;
        [[nodiscard]] status get_or_create_rvalue_reference(TypeRef,TypeRef&) noexcept;
        [[nodiscard]] TypeRef builtin_type_ref(builtin_type) const noexcept;
    private:
        friend class graph_update;
        source_replacement(graph_update& update,source_id source) noexcept:update_(&update),source_(source){}
        graph_update* update_=nullptr; source_id source_{};
    };
    ~graph_update(); graph_update(const graph_update&)=delete; graph_update& operator=(const graph_update&)=delete;
    graph_update(graph_update&&) noexcept; graph_update& operator=(graph_update&&)=delete;
    [[nodiscard]] status replace_source(source_id source, source_replacement& replacement) noexcept;
    [[nodiscard]] const entity_record* find(string_id name) const noexcept;
    [[nodiscard]] const entity_record* find(stable_id id) const noexcept;
    [[nodiscard]] const user_type_record* find(type_handle handle) const noexcept;
    [[nodiscard]] std::span<const enum_value_record> enum_values(type_handle handle) const noexcept;
private:
    friend class graph; friend class graph_build_transaction; friend class graph_build_transaction_test_access;
    graph_update(graph& owner,std::uint64_t generation,std::uint64_t candidate_generation) noexcept;
#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
public:
#endif
    [[nodiscard]] status declare_named_enum(string_id name,source_id source,const enum_build_data& data,stable_id& entity,type_handle& type) noexcept;
    [[nodiscard]] status declare_named_type(string_id name,source_id source,
                                            aggregate_definition_state state,
                                            stable_id& entity,type_handle& type) noexcept;
    [[nodiscard]] status add_anonymous_enum(source_id source,const enum_build_data& data,type_handle& type) noexcept;
#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)
private:
#endif
    [[nodiscard]] status prepare_publish(const source_manager_update&,const string_registry_update&) noexcept;
    void publish_prepared() noexcept; void cancel() noexcept;
    [[nodiscard]] status begin_source_replacement(source_id) noexcept;
    [[nodiscard]] status remove_named_entity_for_testing(stable_id) noexcept;
    [[nodiscard]] status build_contribution(const enum_build_data&,graph::source_contribution&) noexcept;
    [[nodiscard]] status add_delta(source_id,stable_id,const graph::source_contribution&) noexcept;
    [[nodiscard]] status remove_delta(source_id,const graph::source_contribution&) noexcept;
    [[nodiscard]] status materialize(stable_id,string_id) noexcept;
    [[nodiscard]] status assign_type(stable_id,graph::entity_slot&,std::unique_ptr<graph::type_storage>) noexcept;
    [[nodiscard]] status get_or_create_named_type_ref(type_handle,TypeRef&) noexcept;
    [[nodiscard]] status get_or_create_derived(derived_type_kind,TypeRef,std::uint64_t,TypeRef&) noexcept;
    graph::candidate_identity_slot& touch_identity(std::uint32_t);
    graph::candidate_entity_slot& touch_entity(std::uint32_t);
    graph::candidate_type_slot& touch_type(std::uint32_t);
    graph::candidate_source_slot& touch_source(std::uint32_t);
    graph* owner_=nullptr;
    std::vector<std::unique_ptr<graph::type_storage>> owned_types_;
    std::vector<std::uint32_t> changed_identities_,changed_entities_,changed_types_,changed_sources_;
    std::vector<std::uint32_t> claimed_free_type_slots_;
    std::vector<graph::canonical_type_record> added_canonical_types_;
    std::vector<std::pair<std::uint32_t,TypeRef>> added_named_type_refs_;
    std::unordered_map<std::uint32_t,TypeRef> added_named_type_index_;
    std::unordered_map<graph::derived_type_key,TypeRef,graph::derived_type_key_hash> added_derived_type_index_;
    std::uint32_t next_stable_id_=1,next_type_slot_=1;
    std::uint64_t base_generation_=0,candidate_generation_=0;
    status failure_{}; bool prepared_=false,committed_=false;
};
} // namespace cw::server
