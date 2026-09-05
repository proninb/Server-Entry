#include "../server_entry/project/graph/graph_manager.hpp"
#include "../server_entry/project/graph/graph_build_transaction.hpp"
#include "../server_entry/project/graph/type_ref.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <type_traits>

namespace cw::server {

class graph_build_transaction_test_access {
public:
    static const source_manager& sources(const graph_manager& value) noexcept {
        return value.source_manager_state;
    }

    static source_manager_update local_sources(graph_manager& value) noexcept {
        return value.source_manager_state.begin_update();
    }

    static string_registry_update local_strings(graph_manager& value) noexcept {
        return value.string_registry_state.begin_update();
    }

    static status prepare(graph_build_transaction& value) noexcept {
        return value.prepare();
    }

    static graph_build_transaction_state state(
        const graph_build_transaction& value) noexcept {

        return value.state;
    }

    static std::uint64_t source_generation(
        const graph_manager& value) noexcept {

        return value.source_manager_state.generation;
    }

    static std::uint64_t string_generation(
        const graph_manager& value) noexcept {

        return value.string_registry_state.generation;
    }

    static std::uint64_t graph_generation(
        const graph_manager& value) noexcept {

        return value.graph_state.generation;
    }

    static std::size_t contribution_count(
        const graph_manager& value,
        source_id source) noexcept {

        return value.source_contribution_cache_state.contribution_count(source);
    }

    static bool contribution_cache_complete(
        const graph_manager& value) noexcept {

        return value.source_contribution_cache_state.complete();
    }

    static void fail_sources(graph_build_transaction& value) noexcept {
        value.fail_source_prepare = true;
    }

    static void fail_strings(graph_build_transaction& value) noexcept {
        value.fail_string_prepare = true;
    }

    static void fail_graph(graph_build_transaction& value) noexcept {
        value.fail_graph_prepare = true;
    }
};

} // namespace cw::server

namespace {

using namespace cw::server;
using access = graph_build_transaction_test_access;

template <typename T>
concept has_entity_id_field = requires(T value) {
    value.id;
};

template <typename T>
concept has_defining_source_field = requires(T value) {
    value.defining_source;
};

template <typename T>
concept has_aggregate_payload_field = requires(T value) {
    value.aggregate;
};

template <typename T>
concept has_definition_state_field = requires(T value) {
    value.definition_state;
};

template <typename T>
concept has_graph_contribution_count = requires(const T& value) {
    value.contribution_count(source_id{1});
};

static_assert(!has_entity_id_field<entity_entry>);
static_assert(!has_defining_source_field<entity_entry>);
static_assert(!has_aggregate_payload_field<type_entry>);
static_assert(!has_definition_state_field<enum_entry>);
static_assert(!has_graph_contribution_count<graph>);
static_assert(!std::is_constructible_v<stable_id, std::uint32_t>);
static_assert(!std::is_constructible_v<type_handle, std::uint32_t>);
static_assert(!std::is_copy_constructible_v<graph_manager>);
static_assert(!std::is_move_constructible_v<graph_manager>);
static_assert(!std::is_copy_constructible_v<graph_build_transaction>);
static_assert(std::is_nothrow_move_constructible_v<graph_build_transaction>);
static_assert(!std::is_move_assignable_v<graph_build_transaction>);

const std::filesystem::path source_a = LR"(C:\graph-build\a.cpp)";
const std::filesystem::path source_b = LR"(C:\graph-build\b.cpp)";
const std::filesystem::path source_c = LR"(C:\graph-build\c.cpp)";

bool resolve_source(
    graph_build_transaction& transaction,
    const std::filesystem::path& path,
    source_id& output) {

    return transaction.sources().resolve(
        path,
        project_item_role::source,
        output).ok();
}

bool open_source(
    graph_build_transaction& transaction,
    source_id source,
    graph_update::source_replacement& output) {

    return transaction.graph_state().replace_source(
        source,
        output).ok();
}

bool test_project_lifecycle_runtime_gate() {
    graph_manager manager;

    if (!manager.initialize().ok() ||
        manager.state() != project_state::error) {
        return false;
    }

    const graph* runnable = nullptr;

    if (manager.runnable_graph(runnable).ok() ||
        runnable != nullptr) {
        return false;
    }

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        if (manager.state() != project_state::building ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    if (manager.state() != project_state::valid ||
        !manager.runnable_graph(runnable).ok() ||
        runnable != &manager.compiled_graph()) {
        return false;
    }

    {
        auto transaction =
            manager.begin_build(graph_build_mode::incremental);

        access::fail_sources(transaction);

        if (transaction.commit().ok() ||
            access::state(transaction) !=
                graph_build_transaction_state::failed) {
            return false;
        }
    }

    runnable = nullptr;

    return
        manager.state() == project_state::error &&
        !manager.runnable_graph(runnable).ok() &&
        runnable == nullptr;
}

bool test_discard_and_prepared_mutation_rejection() {
    graph_manager manager;

    if (!manager.initialize().ok()) {
        return false;
    }

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        source_id source;
        string_id name;

        if (!resolve_source(transaction, source_a, source) ||
            !transaction.strings().intern("Transient", name).ok()) {
            return false;
        }

        const enum_build_data opaque{
            enum_definition_state::opaque,
            false,
            builtin_type::integer,
            {}
        };

        graph_update::source_replacement replacement;
        stable_id entity;
        type_handle type;

        if (!open_source(transaction, source, replacement) ||
            !replacement.add_named_enum(
                name,
                opaque,
                entity,
                type).ok() ||
            !access::prepare(transaction).ok()) {
            return false;
        }

        string_id rejected;

        if (transaction.strings().intern("Late", rejected).ok() ||
            transaction.sources().add(
                source_b,
                project_item_role::source).ok() ||
            replacement.add_named_enum(
                name,
                opaque,
                entity,
                type).ok()) {
            return false;
        }
    }

    return
        manager.state() == project_state::error &&
        access::sources(manager).source_count() == 0 &&
        manager.strings().size() == 0 &&
        manager.compiled_graph().entity_count() == 0 &&
        manager.compiled_graph().user_type_count() == 0;
}

bool test_stale_source_generation_is_atomic() {
    graph_manager manager;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id candidate_source;
    string_id candidate_name;

    if (!resolve_source(
            transaction,
            source_a,
            candidate_source) ||
        !transaction.strings().intern(
            "Candidate",
            candidate_name).ok()) {
        return false;
    }

    graph_update::source_replacement replacement;
    const enum_build_data opaque{
        enum_definition_state::opaque,
        false,
        builtin_type::integer,
        {}
    };
    stable_id candidate_entity;
    type_handle candidate_type;

    if (!open_source(
            transaction,
            candidate_source,
            replacement) ||
        !replacement.add_named_enum(
            candidate_name,
            opaque,
            candidate_entity,
            candidate_type).ok()) {
        return false;
    }

    auto external =
        access::local_sources(manager);

    if (!external.add(
            source_b,
            project_item_role::source).ok() ||
        !external.commit().ok()) {
        return false;
    }

    if (transaction.commit().ok()) {
        return false;
    }

    return
        manager.state() == project_state::error &&
        access::sources(manager).source_count() == 1 &&
        manager.strings().find("Candidate") == string_id{} &&
        manager.compiled_graph().entity_count() == 0;
}

bool test_forced_prepare_failures_are_atomic() {
    for (const int failure : {0, 1, 2}) {
        graph_manager manager;

        if (!manager.initialize().ok()) {
            return false;
        }

        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        source_id source;
        string_id name;

        if (!resolve_source(transaction, source_a, source) ||
            !transaction.strings().intern(
                "Atomic",
                name).ok()) {
            return false;
        }

        graph_update::source_replacement replacement;
        const enum_build_data opaque{
            enum_definition_state::opaque,
            false,
            builtin_type::integer,
            {}
        };
        stable_id entity;
        type_handle type;

        if (!open_source(transaction, source, replacement) ||
            !replacement.add_named_enum(
                name,
                opaque,
                entity,
                type).ok()) {
            return false;
        }

        if (failure == 0) {
            access::fail_sources(transaction);
        }
        else if (failure == 1) {
            access::fail_strings(transaction);
        }
        else {
            access::fail_graph(transaction);
        }

        if (transaction.commit().ok() ||
            access::state(transaction) !=
                graph_build_transaction_state::failed ||
            access::sources(manager).source_count() != 0 ||
            manager.strings().size() != 0 ||
            manager.compiled_graph().entity_count() != 0 ||
            manager.compiled_graph().user_type_count() != 0) {
            return false;
        }
    }

    return true;
}

bool build_two_names(
    bool reverse,
    std::uint32_t& a_id,
    std::uint32_t& z_id) {

    graph_manager manager;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;

    if (!resolve_source(transaction, source_a, source)) {
        return false;
    }

    string_id a_name;
    string_id z_name;

    if (reverse) {
        if (!transaction.strings().intern("Zeta", z_name).ok() ||
            !transaction.strings().intern("Alpha", a_name).ok()) {
            return false;
        }
    }
    else {
        if (!transaction.strings().intern("Alpha", a_name).ok() ||
            !transaction.strings().intern("Zeta", z_name).ok()) {
            return false;
        }
    }

    graph_update::source_replacement replacement;

    if (!open_source(transaction, source, replacement)) {
        return false;
    }

    const enum_build_data opaque{
        enum_definition_state::opaque,
        false,
        builtin_type::integer,
        {}
    };

    stable_id ignored_id;
    type_handle ignored_type;

    if (reverse) {
        if (!replacement.add_named_enum(
                z_name,
                opaque,
                ignored_id,
                ignored_type).ok() ||
            !replacement.add_named_enum(
                a_name,
                opaque,
                ignored_id,
                ignored_type).ok()) {
            return false;
        }
    }
    else {
        if (!replacement.add_named_enum(
                a_name,
                opaque,
                ignored_id,
                ignored_type).ok() ||
            !replacement.add_named_enum(
                z_name,
                opaque,
                ignored_id,
                ignored_type).ok()) {
            return false;
        }
    }

    if (!transaction.commit().ok()) {
        return false;
    }

    a_id = manager.compiled_graph().find_id(a_name).value();
    z_id = manager.compiled_graph().find_id(z_name).value();

    return
        a_id != 0 &&
        z_id != 0 &&
        a_id != z_id;
}

bool test_deterministic_stable_ids() {
    std::uint32_t a_forward = 0;
    std::uint32_t z_forward = 0;
    std::uint32_t a_reverse = 0;
    std::uint32_t z_reverse = 0;

    return
        build_two_names(
            false,
            a_forward,
            z_forward) &&
        build_two_names(
            true,
            a_reverse,
            z_reverse) &&
        a_forward == a_reverse &&
        z_forward == z_reverse &&
        a_forward < z_forward;
}

bool test_definition_range_and_external_contributions() {
    graph_manager manager;

    if (!manager.initialize().ok()) {
        return false;
    }

    string_id name;
    string_id value_name;
    source_id definition_source;
    source_id declaration_source;
    stable_id identity;
    type_handle type;

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        if (!resolve_source(
                transaction,
                source_a,
                definition_source) ||
            !resolve_source(
                transaction,
                source_b,
                declaration_source) ||
            !transaction.strings().intern(
                "N::State",
                name).ok() ||
            !transaction.strings().intern(
                "Ready",
                value_name).ok()) {
            return false;
        }

        const std::array values{
            enum_value_build{
                value_name,
                {builtin_type::integer, 7}
            }
        };

        const enum_build_data defined{
            enum_definition_state::defined,
            true,
            builtin_type::integer,
            values
        };

        const enum_build_data opaque{
            enum_definition_state::opaque,
            true,
            builtin_type::integer,
            {}
        };

        graph_update::source_replacement definition;
        graph_update::source_replacement declaration;
        stable_id same;
        type_handle same_type;

        if (!open_source(
                transaction,
                definition_source,
                definition) ||
            !open_source(
                transaction,
                declaration_source,
                declaration) ||
            !definition.add_named_enum(
                name,
                defined,
                identity,
                type).ok() ||
            !declaration.add_named_enum(
                name,
                opaque,
                same,
                same_type).ok() ||
            same != identity ||
            same_type != type ||
            !transaction.commit().ok()) {
            return false;
        }

        identity =
            manager.compiled_graph().find_id(name);

        const auto* entity =
            manager.compiled_graph().find(identity);

        if (!entity) {
            return false;
        }

        type = entity->type;
    }

    const auto* defined_type =
        manager.compiled_graph().find(type);

    const auto defined_values =
        manager.compiled_graph().enum_values(type);

    if (!defined_type ||
        defined_type->kind !=
            user_type_kind::enumeration ||
        !defined_type->definition ||
        defined_type->definition.count != 1 ||
        defined_values.size() != 1 ||
        defined_values[0].name != value_name ||
        defined_values[0].bits != 7 ||
        access::contribution_count(
            manager,
            definition_source) != 1 ||
        access::contribution_count(
            manager,
            declaration_source) != 1) {
        return false;
    }

    {
        auto transaction =
            manager.begin_build(
                graph_build_mode::incremental);

        source_id same_source;

        if (!resolve_source(
                transaction,
                source_a,
                same_source) ||
            same_source != definition_source) {
            return false;
        }

        graph_update::source_replacement empty;

        if (!open_source(
                transaction,
                same_source,
                empty) ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    const auto* entity_after_definition_removal =
        manager.compiled_graph().find(identity);

    const auto* opaque_type =
        entity_after_definition_removal
            ? manager.compiled_graph().find(
                  entity_after_definition_removal->type)
            : nullptr;

    if (!entity_after_definition_removal ||
        entity_after_definition_removal->type != type ||
        !opaque_type ||
        opaque_type->definition ||
        !manager.compiled_graph().enum_values(type).empty() ||
        access::contribution_count(
            manager,
            definition_source) != 0 ||
        access::contribution_count(
            manager,
            declaration_source) != 1) {
        return false;
    }

    {
        auto transaction =
            manager.begin_build(
                graph_build_mode::incremental);

        source_id same_source;

        if (!resolve_source(
                transaction,
                source_b,
                same_source) ||
            same_source != declaration_source) {
            return false;
        }

        graph_update::source_replacement empty;

        if (!open_source(
                transaction,
                same_source,
                empty) ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    return
        manager.compiled_graph().find(identity) == nullptr &&
        manager.compiled_graph().find_id(name) == identity &&
        access::contribution_count(
            manager,
            declaration_source) == 0;
}

bool test_defined_empty_range_and_identity_resurrection() {
    graph_manager manager;

    if (!manager.initialize().ok()) {
        return false;
    }

    string_id name;
    source_id source;
    stable_id identity;

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        if (!resolve_source(transaction, source_a, source) ||
            !transaction.strings().intern(
                "Empty",
                name).ok()) {
            return false;
        }

        const enum_build_data defined_empty{
            enum_definition_state::defined,
            false,
            std::nullopt,
            {}
        };

        graph_update::source_replacement replacement;
        stable_id provisional;
        type_handle type;

        if (!open_source(
                transaction,
                source,
                replacement) ||
            !replacement.add_named_enum(
                name,
                defined_empty,
                provisional,
                type).ok() ||
            !transaction.commit().ok()) {
            return false;
        }

        identity =
            manager.compiled_graph().find_id(name);
    }

    const auto* first_entity =
        manager.compiled_graph().find(identity);

    const auto* first_type =
        first_entity
            ? manager.compiled_graph().find(
                  first_entity->type)
            : nullptr;

    if (!first_entity ||
        !first_type ||
        !first_type->definition ||
        first_type->definition.begin == 0 ||
        first_type->definition.count != 0 ||
        !manager.compiled_graph().enum_values(
            first_entity->type).empty()) {
        return false;
    }

    {
        auto transaction =
            manager.begin_build(
                graph_build_mode::incremental);

        source_id same_source;

        if (!resolve_source(
                transaction,
                source_a,
                same_source) ||
            same_source != source) {
            return false;
        }

        graph_update::source_replacement empty;

        if (!open_source(
                transaction,
                same_source,
                empty) ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    if (manager.compiled_graph().find(identity) != nullptr ||
        manager.compiled_graph().find_id(name) != identity) {
        return false;
    }

    {
        auto transaction =
            manager.begin_build(
                graph_build_mode::incremental);

        source_id same_source;
        string_id same_name;

        if (!resolve_source(
                transaction,
                source_a,
                same_source) ||
            !transaction.strings().intern(
                "Empty",
                same_name).ok() ||
            same_source != source ||
            same_name != name) {
            return false;
        }

        const enum_build_data opaque{
            enum_definition_state::opaque,
            false,
            builtin_type::integer,
            {}
        };

        graph_update::source_replacement replacement;
        stable_id resurrected;
        type_handle resurrected_type;

        if (!open_source(
                transaction,
                same_source,
                replacement) ||
            !replacement.add_named_enum(
                same_name,
                opaque,
                resurrected,
                resurrected_type).ok() ||
            resurrected != identity ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    const auto* resurrected =
        manager.compiled_graph().find(identity);

    if (!resurrected) {
        return false;
    }

    {
        auto transaction =
            manager.begin_build(
                graph_build_mode::incremental);

        source_id new_source;
        string_id new_name;

        if (!resolve_source(
                transaction,
                source_b,
                new_source) ||
            !transaction.strings().intern(
                "Different",
                new_name).ok()) {
            return false;
        }

        const enum_build_data opaque{
            enum_definition_state::opaque,
            false,
            builtin_type::integer,
            {}
        };

        graph_update::source_replacement replacement;
        stable_id new_id;
        type_handle new_type;

        if (!open_source(
                transaction,
                new_source,
                replacement) ||
            !replacement.add_named_enum(
                new_name,
                opaque,
                new_id,
                new_type).ok() ||
            !transaction.commit().ok()) {
            return false;
        }

        if (manager.compiled_graph().find_id(
                new_name) == identity) {
            return false;
        }
    }

    return
        manager.compiled_graph().find_id(name) == identity;
}

bool test_aggregate_members_and_modifier_order() {
    graph_manager manager;

    if (!manager.initialize().ok()) {
        return false;
    }

    string_id type_name;
    string_id member_name;
    stable_id identity;
    type_handle type;

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        source_id source;

        if (!resolve_source(transaction, source_a, source) ||
            !transaction.strings().intern(
                "Aggregate",
                type_name).ok() ||
            !transaction.strings().intern(
                "value",
                member_name).ok()) {
            return false;
        }

        graph_update::source_replacement replacement;

        if (!open_source(
                transaction,
                source,
                replacement) ||
            !replacement.add_named_type(
                type_name,
                aggregate_definition_state::defined,
                identity,
                type).ok()) {
            return false;
        }

        const std::array modifiers{
            type_modifier_build{
                derived_type_kind::array,
                4
            },
            type_modifier_build{
                derived_type_kind::pointer,
                0
            }
        };

        const std::array members{
            member_build{
                member_name,
                builtin_type::integer,
                {},
                0,
                2
            }
        };

        if (!replacement.define_members(
                type,
                members,
                modifiers).ok() ||
            !transaction.commit().ok()) {
            return false;
        }

        identity =
            manager.compiled_graph().find_id(
                type_name);

        const auto* entity =
            manager.compiled_graph().find(identity);

        if (!entity) {
            return false;
        }

        type = entity->type;
    }

    const auto* record =
        manager.compiled_graph().find(type);

    const auto members =
        manager.compiled_graph().members(type);

    if (!record ||
        record->kind != user_type_kind::aggregate ||
        !record->definition ||
        record->definition.count != 1 ||
        members.size() != 1 ||
        members[0].name != member_name ||
        manager.compiled_graph().find_member(
            type,
            member_name).value() != 1) {
        return false;
    }

    const auto* outer =
        manager.compiled_graph().derived(
            members[0].type);

    if (!outer ||
        outer->kind != derived_type_kind::pointer ||
        outer->payload != 0) {
        return false;
    }

    const auto* inner =
        manager.compiled_graph().derived(
            outer->child);

    if (!inner ||
        inner->kind != derived_type_kind::array ||
        inner->payload != 4) {
        return false;
    }

    builtin_type base{};

    return
        manager.compiled_graph().builtin(
            inner->child,
            base) &&
        base == builtin_type::integer;
}

bool test_pending_named_member_and_dangling_guard() {
    graph_manager manager;

    if (!manager.initialize().ok()) {
        return false;
    }

    string_id a_name;
    string_id b_name;
    string_id member_name;
    type_handle b_type;

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        source_id source;

        if (!resolve_source(transaction, source_a, source) ||
            !transaction.strings().intern("A", a_name).ok() ||
            !transaction.strings().intern("B", b_name).ok() ||
            !transaction.strings().intern("b", member_name).ok()) {
            return false;
        }

        graph_update::source_replacement replacement;

        if (!open_source(
                transaction,
                source,
                replacement)) {
            return false;
        }

        stable_id a_id;
        stable_id b_id;
        type_handle a_type;

        if (!replacement.add_named_type(
                a_name,
                aggregate_definition_state::defined,
                a_id,
                a_type).ok()) {
            return false;
        }

        const std::array modifiers{
            type_modifier_build{
                derived_type_kind::pointer,
                0
            }
        };

        const std::array members{
            member_build{
                member_name,
                std::nullopt,
                b_name,
                0,
                1
            }
        };

        if (!replacement.define_members(
                a_type,
                members,
                modifiers).ok() ||
            !replacement.add_named_type(
                b_name,
                aggregate_definition_state::declared,
                b_id,
                b_type).ok() ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    const auto* a_entity =
        manager.compiled_graph().find(a_name);

    const auto a_members =
        a_entity
            ? manager.compiled_graph().members(
                  a_entity->type)
            : std::span<const member_record>{};

    if (!a_entity ||
        a_members.size() != 1) {
        return false;
    }

    const auto* pointer =
        manager.compiled_graph().derived(
            a_members[0].type);

    type_handle resolved{};

    if (!pointer ||
        pointer->kind != derived_type_kind::pointer ||
        !manager.compiled_graph().named(
            pointer->child,
            resolved) ||
        resolved != b_type) {
        return false;
    }

    graph_manager rejected;

    if (!rejected.initialize().ok()) {
        return false;
    }

    auto invalid =
        rejected.begin_build(
            graph_build_mode::rebuild);

    source_id invalid_source;
    string_id invalid_a;
    string_id missing_b;
    string_id invalid_member;

    if (!resolve_source(
            invalid,
            source_b,
            invalid_source) ||
        !invalid.strings().intern(
            "InvalidA",
            invalid_a).ok() ||
        !invalid.strings().intern(
            "MissingB",
            missing_b).ok() ||
        !invalid.strings().intern(
            "member",
            invalid_member).ok()) {
        return false;
    }

    graph_update::source_replacement invalid_replacement;
    stable_id invalid_id;
    type_handle invalid_type;

    if (!open_source(
            invalid,
            invalid_source,
            invalid_replacement) ||
        !invalid_replacement.add_named_type(
            invalid_a,
            aggregate_definition_state::defined,
            invalid_id,
            invalid_type).ok()) {
        return false;
    }

    const std::array invalid_members{
        member_build{
            invalid_member,
            std::nullopt,
            missing_b,
            0,
            0
        }
    };

    if (!invalid_replacement.define_members(
            invalid_type,
            invalid_members,
            {}).ok()) {
        return false;
    }

    return
        !invalid.commit().ok() &&
        rejected.state() == project_state::error &&
        rejected.compiled_graph().entity_count() == 0 &&
        rejected.compiled_graph().user_type_count() == 0;
}

bool test_incremental_handle_and_typeref_preservation() {
    graph_manager manager;

    if (!manager.initialize().ok()) {
        return false;
    }

    string_id type_name;
    string_id member_name;
    source_id source;
    stable_id identity;
    type_handle original_type;
    TypeRef original_member_type;

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        if (!resolve_source(transaction, source_a, source) ||
            !transaction.strings().intern(
                "StableType",
                type_name).ok() ||
            !transaction.strings().intern(
                "p",
                member_name).ok()) {
            return false;
        }

        graph_update::source_replacement replacement;
        stable_id provisional;

        if (!open_source(
                transaction,
                source,
                replacement) ||
            !replacement.add_named_type(
                type_name,
                aggregate_definition_state::defined,
                provisional,
                original_type).ok()) {
            return false;
        }

        const std::array modifiers{
            type_modifier_build{
                derived_type_kind::pointer,
                0
            }
        };

        const std::array members{
            member_build{
                member_name,
                builtin_type::integer,
                {},
                0,
                1
            }
        };

        if (!replacement.define_members(
                original_type,
                members,
                modifiers).ok() ||
            !transaction.commit().ok()) {
            return false;
        }

        identity =
            manager.compiled_graph().find_id(
                type_name);

        const auto* entity =
            manager.compiled_graph().find(identity);

        const auto values =
            entity
                ? manager.compiled_graph().members(
                      entity->type)
                : std::span<const member_record>{};

        if (!entity ||
            values.size() != 1) {
            return false;
        }

        original_type = entity->type;
        original_member_type = values[0].type;
    }

    {
        auto transaction =
            manager.begin_build(
                graph_build_mode::incremental);

        source_id same_source;
        string_id same_type_name;
        string_id same_member_name;

        if (!resolve_source(
                transaction,
                source_a,
                same_source) ||
            !transaction.strings().intern(
                "StableType",
                same_type_name).ok() ||
            !transaction.strings().intern(
                "p",
                same_member_name).ok() ||
            same_source != source ||
            same_type_name != type_name ||
            same_member_name != member_name) {
            return false;
        }

        graph_update::source_replacement replacement;
        stable_id same_identity;
        type_handle same_type;

        if (!open_source(
                transaction,
                same_source,
                replacement) ||
            !replacement.add_named_type(
                same_type_name,
                aggregate_definition_state::defined,
                same_identity,
                same_type).ok() ||
            same_identity != identity ||
            same_type != original_type) {
            return false;
        }

        const std::array modifiers{
            type_modifier_build{
                derived_type_kind::pointer,
                0
            }
        };

        const std::array members{
            member_build{
                same_member_name,
                builtin_type::integer,
                {},
                0,
                1
            }
        };

        if (!replacement.define_members(
                same_type,
                members,
                modifiers).ok() ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    const auto* entity =
        manager.compiled_graph().find(identity);

    const auto values =
        entity
            ? manager.compiled_graph().members(
                  entity->type)
            : std::span<const member_record>{};

    return
        entity &&
        entity->type == original_type &&
        values.size() == 1 &&
        values[0].type == original_member_type &&
        manager.compiled_graph().derived(
            original_member_type) != nullptr;
}

bool test_compiled_checkpoint_roundtrip_and_fail_closed_load() {
    graph_manager saved;

    if (!saved.initialize(
            {abi_target::windows_x64, 16}).ok()) {
        return false;
    }

    string_id name;
    string_id value_name;
    stable_id identity;
    type_handle type;

    {
        auto transaction =
            saved.begin_build(graph_build_mode::rebuild);

        source_id source;

        if (!resolve_source(
                transaction,
                source_a,
                source) ||
            !transaction.strings().intern(
                "Persisted",
                name).ok() ||
            !transaction.strings().intern(
                "Answer",
                value_name).ok()) {
            return false;
        }

        const std::array values{
            enum_value_build{
                value_name,
                {
                    builtin_type::unsigned_long_long_integer,
                    42
                }
            }
        };

        const enum_build_data data{
            enum_definition_state::defined,
            true,
            builtin_type::unsigned_long_long_integer,
            values
        };

        graph_update::source_replacement replacement;
        stable_id provisional;

        if (!open_source(
                transaction,
                source,
                replacement) ||
            !replacement.add_named_enum(
                name,
                data,
                provisional,
                type).ok() ||
            !transaction.commit().ok()) {
            return false;
        }

        identity =
            saved.compiled_graph().find_id(name);

        const auto* entity =
            saved.compiled_graph().find(identity);

        if (!entity) {
            return false;
        }

        type = entity->type;
    }

    const auto file =
        std::filesystem::temp_directory_path() /
        L"cw_graph_manager_v2_checkpoint.bin";

    std::error_code ignored;
    std::filesystem::remove(file, ignored);

    if (!saved.save_compiled_checkpoint(file).ok()) {
        return false;
    }

    graph_manager loaded;

    if (!loaded.initialize().ok() ||
        !loaded.load_compiled_checkpoint(file).ok() ||
        loaded.state() != project_state::valid ||
        access::contribution_cache_complete(loaded)) {
        std::filesystem::remove(file, ignored);
        return false;
    }

    const auto loaded_name =
        loaded.strings().find("Persisted");

    const auto loaded_value_name =
        loaded.strings().find("Answer");

    const auto loaded_identity =
        loaded.compiled_graph().find_id(
            loaded_name);

    const auto* loaded_entity =
        loaded.compiled_graph().find(
            loaded_identity);

    const auto* loaded_type =
        loaded_entity
            ? loaded.compiled_graph().find(
                  loaded_entity->type)
            : nullptr;

    const auto loaded_values =
        loaded_entity
            ? loaded.compiled_graph().enum_values(
                  loaded_entity->type)
            : std::span<const enum_value_record>{};

    if (loaded_name != name ||
        loaded_value_name != value_name ||
        loaded_identity != identity ||
        !loaded_entity ||
        loaded_entity->type != type ||
        !loaded_type ||
        !loaded_type->definition ||
        loaded_values.size() != 1 ||
        loaded_values[0].name != value_name ||
        loaded_values[0].bits != 42 ||
        loaded.compiled_graph().abi().pack != 16) {
        std::filesystem::remove(file, ignored);
        return false;
    }

    {
        std::fstream corrupt{
            file,
            std::ios::binary |
            std::ios::in |
            std::ios::out
        };

        if (!corrupt) {
            std::filesystem::remove(file, ignored);
            return false;
        }

        const char bad = 'X';
        corrupt.write(&bad, 1);
    }

    if (loaded.load_compiled_checkpoint(file).ok() ||
        loaded.state() != project_state::error ||
        loaded.compiled_graph().find(identity) == nullptr) {
        std::filesystem::remove(file, ignored);
        return false;
    }

    const graph* runnable = nullptr;

    const bool blocked =
        !loaded.runnable_graph(runnable).ok() &&
        runnable == nullptr;

    std::filesystem::remove(file, ignored);
    return blocked;
}

} // namespace

int main() {
    const struct test_case {
        const char* name;
        bool (*run)();
    } tests[] = {
        {"project lifecycle/runtime gate", test_project_lifecycle_runtime_gate},
        {"discard/prepared rejection", test_discard_and_prepared_mutation_rejection},
        {"stale Source generation", test_stale_source_generation_is_atomic},
        {"forced prepare failures", test_forced_prepare_failures_are_atomic},
        {"deterministic stable IDs", test_deterministic_stable_ids},
        {"definition range/external contributions", test_definition_range_and_external_contributions},
        {"defined-empty/resurrection", test_defined_empty_range_and_identity_resurrection},
        {"aggregate modifiers", test_aggregate_members_and_modifier_order},
        {"pending named member/dangling guard", test_pending_named_member_and_dangling_guard},
        {"incremental handle/TypeRef preservation", test_incremental_handle_and_typeref_preservation},
        {"compiled checkpoint/fail-closed load", test_compiled_checkpoint_roundtrip_and_fail_closed_load}
    };

    for (const auto& test : tests) {
        if (!test.run()) {
            std::cerr << "FAILED: " << test.name << '\n';
            return 1;
        }

        std::cout << "PASS: " << test.name << '\n';
    }

    return 0;
}
