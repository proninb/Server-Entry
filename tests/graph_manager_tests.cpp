#include "../server_entry/project/graph/graph_manager.hpp"
#include "../server_entry/project/graph/graph_build_transaction.hpp"
#include "../server_entry/project/graph/type_ref.hpp"

#include <concepts>
#include <filesystem>
#include <array>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <fstream>

namespace cw::server
{
class graph_build_transaction_test_access
{
public:
    static const source_manager& sources(const graph_manager& value) noexcept { return value.sources_; }
    static source_manager_update local_sources(graph_manager& value) noexcept { return value.sources_.begin_update(); }
    static string_registry_update local_strings(graph_manager& value) noexcept { return value.strings_.begin_update(); }
    static status prepare(graph_build_transaction& value) noexcept { return value.prepare(); }
    static std::uint64_t source_generation(const graph_manager& value) noexcept { return value.sources_.generation_; }
    static std::uint64_t string_generation(const graph_manager& value) noexcept { return value.strings_.generation_; }
    static std::uint64_t graph_generation(const graph_manager& value) noexcept { return value.graph_.generation_; }
    static graph_build_transaction_state state(const graph_build_transaction& value) noexcept { return value.state_; }
    static void fail_sources(graph_build_transaction& value) noexcept { value.fail_source_prepare_ = true; }
    static void fail_strings(graph_build_transaction& value) noexcept { value.fail_string_prepare_ = true; }
    static void fail_graph(graph_build_transaction& value) noexcept { value.fail_graph_prepare_ = true; }
    static status remove_candidate_entity(graph_update& value, stable_id id) noexcept
    { return value.remove_named_entity_for_testing(id); }
    static void advance_graph_only(graph_manager& manager)
    {
        auto sources = manager.sources_.begin_update();
        auto strings = manager.strings_.begin_update();
        auto graph = manager.graph_.begin_update();
        if (!graph.prepare_publish(sources, strings).ok()) std::abort();
        graph.publish_prepared();
    }
};
} // namespace cw::server

namespace
{
using namespace cw::server;
using access = graph_build_transaction_test_access;

template <typename T> concept exposes_sources = requires(const T& value) { value.sources(); };
template <typename T> concept exposes_mutable_strings = requires(T& value) {
    { value.strings() } -> std::same_as<string_registry&>;
};
static_assert(!std::is_copy_constructible_v<graph_manager>);
static_assert(!std::is_move_constructible_v<graph_manager>);
static_assert(!std::is_copy_constructible_v<graph_build_transaction>);
static_assert(std::is_nothrow_move_constructible_v<graph_build_transaction>);
static_assert(!std::is_move_assignable_v<graph_build_transaction>);
static_assert(!exposes_sources<graph_manager>);
static_assert(!exposes_mutable_strings<graph_manager>);
static_assert(!std::is_constructible_v<stable_id, std::uint32_t>);
static_assert(!std::is_constructible_v<type_handle, std::uint32_t>);

const std::filesystem::path source_a = LR"(C:\graph-build\a.cpp)";
const std::filesystem::path source_b = LR"(C:\graph-build\b.cpp)";
const std::filesystem::path source_c = LR"(C:\graph-build\c.cpp)";

bool add_candidate(graph_build_transaction& transaction, const std::filesystem::path& path,
                   std::string_view text, string_id& id)
{
    return transaction.sources().add(path, project_item_role::source).ok() &&
           transaction.strings().intern(text, id).ok();
}

bool test_project_lifecycle_runtime_gate()
{
    graph_manager manager;
    if (!manager.initialize().ok() || manager.state() != project_state::error)
        return false;
    const graph* runnable = nullptr;
    if (manager.runnable_graph(runnable).ok() || runnable != nullptr) return false;

    {
        auto transaction = manager.begin_build();
        if (manager.state() != project_state::building ||
            manager.runnable_graph(runnable).ok() || runnable != nullptr ||
            !transaction.commit().ok()) return false;
    }
    if (manager.state() != project_state::valid ||
        !manager.runnable_graph(runnable).ok()) return false;

    {
        auto transaction = manager.begin_build();
        access::fail_sources(transaction);
        if (transaction.commit().ok()) return false;
    }
    if (manager.state() != project_state::error ||
        manager.runnable_graph(runnable).ok() || runnable != nullptr)
        return false;

    // The physical storage remains inspectable, but is not runnable.
    (void)manager.compiled_graph();
    return true;
}

graph_update::source_replacement open_source(graph_update& graph, source_id source)
{
    graph_update::source_replacement replacement;
    if (!graph.replace_source(source, replacement).ok()) std::abort();
    return replacement;
}

bool test_discard_and_prepared_mutation_rejection()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    const auto source_generation = access::source_generation(manager);
    const auto string_generation = access::string_generation(manager);
    const auto graph_generation = access::graph_generation(manager);
    {
        auto transaction = manager.begin_build();
        string_id id;
        if (!add_candidate(transaction, source_a, "active", id)) return false;
        if (manager.strings().find("active") || !access::sources(manager).sources().empty()) return false;
    }
    if (manager.strings().size() != 0 || !access::sources(manager).sources().empty()) return false;
    {
        auto transaction = manager.begin_build();
        string_id id;
        if (!add_candidate(transaction, source_a, "prepared", id) || !access::prepare(transaction).ok()) return false;
        if (access::state(transaction) != graph_build_transaction_state::prepared) return false;
        string_id rejected;
        if (transaction.strings().intern("late", rejected).ok() ||
            transaction.sources().add(source_b, project_item_role::source).ok()) return false;
        type_handle rejected_type;
        const enum_build_data rejected_enum{enum_definition_state::defined, false,
                                             std::nullopt, {}};
        if (transaction.graph_state().add_anonymous_enum(
                source_id{1}, rejected_enum, rejected_type).ok()) return false;
    }
    return manager.strings().size() == 0 && access::sources(manager).sources().empty() &&
           access::source_generation(manager) == source_generation &&
           access::string_generation(manager) == string_generation &&
           access::graph_generation(manager) == graph_generation;
}

bool test_success_identity_and_second_commit()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    string_id first_string;
    auto first = manager.begin_build();
    if (!add_candidate(first, source_a, "stable", first_string) || !first.commit().ok() || first.commit().ok()) return false;
    const auto* first_source = access::sources(manager).find(source_id{1});
    const auto before = manager.strings().get(first_string);
    if (first_source == nullptr || !before) return false;
    const auto* bytes = before->data();
    auto second = manager.begin_build();
    string_id second_string;
    if (!add_candidate(second, source_b, "second", second_string) || !second.commit().ok()) return false;
    const auto after = manager.strings().get(first_string);
    return after && after->data() == bytes && *after == "stable" &&
           access::sources(manager).find(source_id{1}) != nullptr &&
           access::sources(manager).find(source_id{1})->path == source_a &&
           access::sources(manager).find(source_id{2}) != nullptr;
}

bool test_forced_prepare_failures_are_atomic()
{
    for (const int failure : {0, 1, 2})
    {
        graph_manager manager;
        if (!manager.initialize().ok()) return false;
        auto transaction = manager.begin_build();
        string_id id;
        if (!add_candidate(transaction, source_a, "atomic", id)) return false;
        const enum_build_data graph_candidate{enum_definition_state::opaque, false,
                                              builtin_type::integer, {}};
        stable_id graph_id; type_handle graph_type;
        if (!transaction.graph_state().declare_named_enum(
                id, source_id{1}, graph_candidate, graph_id, graph_type).ok()) return false;
        const auto source_generation = access::source_generation(manager);
        const auto string_generation = access::string_generation(manager);
        const auto graph_generation = access::graph_generation(manager);
        if (failure == 0) access::fail_sources(transaction);
        else if (failure == 1) access::fail_strings(transaction);
        else access::fail_graph(transaction);
        if (transaction.commit().ok() || access::state(transaction) != graph_build_transaction_state::failed) return false;
        if (!access::sources(manager).sources().empty() || manager.strings().size() != 0 ||
            manager.compiled_graph().entity_count() != 0 ||
            access::source_generation(manager) != source_generation ||
            access::string_generation(manager) != string_generation ||
            access::graph_generation(manager) != graph_generation) return false;
        string_id rejected;
        if (transaction.strings().intern("late", rejected).ok() ||
            transaction.sources().add(source_b, project_item_role::source).ok()) return false;
    }
    return true;
}

bool test_stale_generations_are_atomic()
{
    for (const bool stale_strings : {false, true})
    {
        graph_manager manager;
        if (!manager.initialize().ok()) return false;
        auto transaction = manager.begin_build();
        string_id candidate;
        if (!add_candidate(transaction, source_a, "candidate", candidate)) return false;
        if (stale_strings)
        {
            auto local = access::local_strings(manager); string_id external;
            if (!local.intern("external", external).ok() || !local.commit().ok()) return false;
        }
        else
        {
            auto local = access::local_sources(manager);
            if (!local.add(source_b, project_item_role::source).ok() || !local.commit().ok()) return false;
        }
        const auto source_count = access::sources(manager).sources().size();
        const auto string_count = manager.strings().size();
        if (transaction.commit().ok() || access::sources(manager).sources().size() != source_count ||
            manager.strings().size() != string_count || manager.strings().find("candidate")) return false;
    }
    return true;
}

bool test_move_and_local_commits()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto original = manager.begin_build(); string_id id;
    if (!add_candidate(original, source_a, "moved", id)) return false;
    auto moved = std::move(original); string_id rejected;
    type_handle rejected_type;
    const enum_build_data rejected_enum{enum_definition_state::defined, false,
                                         std::nullopt, {}};
    if (original.commit().ok() || original.strings().intern("x", rejected).ok() ||
        original.sources().add(source_b, project_item_role::source).ok() || !moved.commit().ok()) return false;
    if (original.graph_state().add_anonymous_enum(
            source_id{1}, rejected_enum, rejected_type).ok()) return false;
    source_manager sources; if (!sources.initialize().ok()) return false;
    auto source_update = sources.begin_update();
    if (!source_update.add(source_a, project_item_role::source).ok() || !source_update.commit().ok()) return false;
    string_registry strings; if (!strings.initialize().ok()) return false;
    auto string_update = strings.begin_update(); string_id local_id;
    return string_update.intern("local", local_id).ok() && string_update.commit().ok() && strings.find("local") == local_id;
}

bool test_type_values_and_enum_core()
{
    if (stable_id{}.valid() || type_handle{}.valid()) return false;
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    graph_update::source_replacement type_replacement;
    if (!transaction.graph_state().replace_source(source_id{1},type_replacement).ok()) return false;
    const auto integer=type_replacement.builtin_type_ref(builtin_type::integer);
    TypeRef pointer,reference;
    if (!type_replacement.get_or_create_pointer(integer,pointer).ok()||
        !type_replacement.get_or_create_array(pointer,4,reference).ok()) return false;
    if (!transaction.sources().add(source_a, project_item_role::source).ok()) return false;
    string_id enum_name, value_name, anonymous_name;
    if (!transaction.strings().intern("N::E", enum_name).ok() ||
        !transaction.strings().intern("A", value_name).ok() ||
        !transaction.strings().intern("AnonymousA", anonymous_name).ok()) return false;
    const enum_value_build named_values[] = {
        {value_name, {builtin_type::integer, 1}}};
    const enum_build_data named_data{enum_definition_state::defined, false,
                                     builtin_type::integer, named_values};
    stable_id entity; type_handle named_type;
    if (!transaction.graph_state().declare_named_enum(
            enum_name, source_id{1}, named_data, entity, named_type).ok()) return false;
    const enum_value_build anonymous_values[] = {
        {anonymous_name, {builtin_type::integer, 7}}};
    const enum_build_data anonymous_data{enum_definition_state::defined, false,
                                         std::nullopt, anonymous_values};
    type_handle anonymous_type;
    if (!transaction.graph_state().add_anonymous_enum(
            source_id{1}, anonymous_data, anonymous_type).ok()) return false;
    if (manager.compiled_graph().find(entity) != nullptr ||
        manager.compiled_graph().find(named_type) != nullptr) return false;
    if (!transaction.commit().ok()) return false;
    const auto* entity_record = manager.compiled_graph().find(entity);
    const auto* named_record = manager.compiled_graph().find(named_type);
    const auto* anonymous_record = manager.compiled_graph().find(anonymous_type);
    const auto values = manager.compiled_graph().enum_values(named_type);
    return entity_record && entity_record->kind == entity_kind::enum_type &&
           entity_record->type == named_type && named_record && anonymous_record &&
           named_record->kind == user_type_kind::enumeration &&
           named_record->enumeration.definition_state == enum_definition_state::defined &&
           named_record->enumeration.underlying == builtin_type::integer &&
           !named_record->enumeration.scoped && values.size() == 1 && values[0].bits == 1 &&
           anonymous_record->enumeration.definition_state == enum_definition_state::defined &&
           manager.compiled_graph().entity_count() == 1 &&
           manager.compiled_graph().user_type_count() == 2;
}

bool test_enum_transitions_identity_and_abi()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    string_id name, value_name;
    stable_id identity; type_handle opaque_handle;
    {
        auto transaction = manager.begin_build();
        if (!transaction.sources().add(source_a, project_item_role::source).ok() ||
            !transaction.strings().intern("M", name).ok() ||
            !transaction.strings().intern("A", value_name).ok()) return false;
        const enum_build_data opaque{enum_definition_state::opaque, true, std::nullopt, {}};
        if (!transaction.graph_state().declare_named_enum(
                name, source_id{1}, opaque, identity, opaque_handle).ok() ||
            !transaction.commit().ok()) return false;
    }
    const auto* opaque = manager.compiled_graph().find(opaque_handle);
    if (!opaque || opaque->enumeration.definition_state != enum_definition_state::opaque ||
        opaque->enumeration.underlying != builtin_type::integer ||
        !opaque->enumeration.scoped || !manager.compiled_graph().enum_values(opaque_handle).empty() ||
        manager.compiled_graph().contribution_count(source_id{1}) != 1)
        return false;

    type_handle defined_handle;
    {
        auto transaction = manager.begin_build();
        if (!transaction.sources().add(source_b, project_item_role::source).ok()) return false;
        const enum_value_build values[] = {{value_name, {builtin_type::integer, 1}}};
        const enum_build_data defined{enum_definition_state::defined, true, std::nullopt, values};
        stable_id same;
        if (!transaction.graph_state().declare_named_enum(
                name, source_id{2}, defined, same, defined_handle).ok() || same != identity ||
            !transaction.commit().ok()) return false;
    }
    const auto* moved = manager.compiled_graph().find(identity);
    if (!moved || moved->defining_source != source_id{2} || moved->type != defined_handle ||
        defined_handle != opaque_handle || manager.compiled_graph().contribution_count(source_id{1}) != 1 ||
        manager.compiled_graph().contribution_count(source_id{2}) != 1) return false;

    type_handle downgraded;
    {
        auto transaction = manager.begin_build();
        const enum_build_data opaque_again{enum_definition_state::opaque, true, std::nullopt, {}};
        stable_id same;
        if (!transaction.graph_state().declare_named_enum(
                name, source_id{2}, opaque_again, same, downgraded).ok() || same != identity ||
            !transaction.commit().ok()) return false;
    }
    const auto* downgraded_record = manager.compiled_graph().find(downgraded);
    if (!downgraded_record ||
        downgraded_record->enumeration.definition_state != enum_definition_state::opaque)
        return false;

    builtin_type selected{};
    const integral_constant abi_values[] = {
        {builtin_type::unsigned_long_long_integer, 0xffffffffULL}};
    return select_unscoped_enum_underlying(abi_values, abi_configuration{}, selected).ok() &&
           selected == builtin_type::unsigned_integer;
}

bool test_enum_redeclaration_failures_and_nonreuse()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    stable_id first; type_handle first_type; string_id name, value_name;
    {
        auto transaction = manager.begin_build();
        if (!transaction.sources().add(source_a, project_item_role::source).ok() ||
            !transaction.strings().intern("E", name).ok() ||
            !transaction.strings().intern("A", value_name).ok()) return false;
        const enum_value_build values[] = {{value_name, {builtin_type::integer, 1}}};
        const enum_build_data definition{enum_definition_state::defined, false,
                                         builtin_type::integer, values};
        if (!transaction.graph_state().declare_named_enum(
                name, source_id{1}, definition, first, first_type).ok() ||
            !transaction.commit().ok()) return false;
    }
    {
        auto transaction = manager.begin_build();
        const enum_value_build values[] = {{value_name, {builtin_type::integer, 1}}};
        const enum_build_data definition{enum_definition_state::defined, false,
                                         builtin_type::integer, values};
        stable_id ignored; type_handle ignored_type;
        if (!transaction.graph_state().declare_named_enum(name, source_id{1}, definition,
                                                          ignored, ignored_type).ok()) return false;
        if (transaction.graph_state().declare_named_enum(name, source_id{1}, definition,
                                                         ignored, ignored_type).ok() ||
            transaction.commit().ok()) return false;
    }
    {
        auto transaction = manager.begin_build();
        const enum_build_data scoped{enum_definition_state::opaque, true,
                                     builtin_type::integer, {}};
        const enum_build_data unscoped{enum_definition_state::opaque, false,
                                       builtin_type::integer, {}};
        stable_id ignored; type_handle ignored_type;
        if (!transaction.graph_state().declare_named_enum(name, source_id{1}, scoped,
                                                          ignored, ignored_type).ok() ||
            transaction.graph_state().declare_named_enum(name, source_id{1}, unscoped,
                                                          ignored, ignored_type).ok() ||
            transaction.commit().ok()) return false;
    }
    {
        auto transaction = manager.begin_build();
        const enum_build_data first_base{enum_definition_state::opaque, false,
                                         builtin_type::integer, {}};
        const enum_build_data second_base{enum_definition_state::opaque, false,
                                          builtin_type::short_integer, {}};
        stable_id ignored; type_handle ignored_type;
        if (!transaction.graph_state().declare_named_enum(name, source_id{1}, first_base,
                                                          ignored, ignored_type).ok() ||
            transaction.graph_state().declare_named_enum(name, source_id{1}, second_base,
                                                          ignored, ignored_type).ok() ||
            transaction.commit().ok()) return false;
    }
    {
        auto transaction = manager.begin_build();
        auto source = open_source(transaction.graph_state(), source_id{1});
        (void)source;
        if (!transaction.commit().ok() || manager.compiled_graph().find(first)) return false;
    }
    stable_id second; type_handle second_type;
    {
        auto transaction = manager.begin_build();
        const enum_build_data opaque{enum_definition_state::opaque, false,
                                     builtin_type::integer, {}};
        if (!transaction.graph_state().declare_named_enum(name, source_id{1}, opaque,
                                                          second, second_type).ok() ||
            !transaction.commit().ok()) return false;
    }
    return second.value() > first.value();
}

bool test_stale_graph_and_graph_prepare_atomicity()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    if (!transaction.sources().add(source_a, project_item_role::source).ok()) return false;
    string_id name;
    if (!transaction.strings().intern("stale", name).ok()) return false;
    const enum_build_data opaque{enum_definition_state::opaque, false,
                                 builtin_type::integer, {}};
    stable_id id; type_handle type;
    if (!transaction.graph_state().declare_named_enum(name, source_id{1}, opaque, id, type).ok())
        return false;
    access::advance_graph_only(manager);
    return !transaction.commit().ok() && manager.strings().find("stale") == string_id{} &&
           manager.compiled_graph().entity_count() == 0;
}

bool test_canonical_contribution_recomposition()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    string_id name, value;
    stable_id id; type_handle slot;
    {
        auto transaction = manager.begin_build();
        if (!transaction.sources().add(source_a, project_item_role::source).ok() ||
            !transaction.sources().add(source_b, project_item_role::source).ok() ||
            !transaction.strings().intern("Canonical", name).ok() ||
            !transaction.strings().intern("Value", value).ok()) return false;
        const enum_value_build values[] = {{value, {builtin_type::integer, 1}}};
        const enum_build_data definition{enum_definition_state::defined, false,
                                          builtin_type::integer, values};
        const enum_build_data opaque{enum_definition_state::opaque, false,
                                      builtin_type::integer, {}};
        auto b = open_source(transaction.graph_state(), source_id{2});
        auto a = open_source(transaction.graph_state(), source_id{1});
        stable_id same; type_handle same_slot;
        if (!b.add_named_enum(name, opaque, id, slot).ok() ||
            !a.add_named_enum(name, definition, same, same_slot).ok() ||
            same != id || same_slot != slot || !transaction.commit().ok()) return false;
    }
    const auto* defined = manager.compiled_graph().find(id);
    if (!defined || manager.compiled_graph().find(name) != defined ||
        defined->defining_source != source_id{1} || defined->type != slot ||
        manager.compiled_graph().find(slot)->enumeration.definition_state !=
            enum_definition_state::defined) return false;
    const auto type_count = manager.compiled_graph().user_type_count();
    {
        auto transaction = manager.begin_build();
        auto a = open_source(transaction.graph_state(), source_id{1});
        (void)a;
        if (!transaction.commit().ok()) return false;
    }
    const auto* opaque_entity = manager.compiled_graph().find(id);
    const auto* opaque_type = opaque_entity ? manager.compiled_graph().find(opaque_entity->type) : nullptr;
    if (!opaque_entity || opaque_entity->defining_source || opaque_entity->type != slot ||
        !opaque_type || opaque_type->enumeration.definition_state != enum_definition_state::opaque ||
        !opaque_type->enumeration.fixed_underlying ||
        manager.compiled_graph().user_type_count() != type_count) return false;
    {
        auto transaction = manager.begin_build();
        auto b = open_source(transaction.graph_state(), source_id{2});
        (void)b;
        if (transaction.graph_state().find(id) != nullptr || !transaction.commit().ok()) return false;
    }
    return manager.compiled_graph().find(id) == nullptr &&
           manager.compiled_graph().find(slot) == nullptr;
}

bool test_fixed_underlying_and_constant_boundary()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    if (!transaction.sources().add(source_a, project_item_role::source).ok() ||
        !transaction.sources().add(source_b, project_item_role::source).ok()) return false;
    string_id name, value;
    if (!transaction.strings().intern("Fixed", name).ok() ||
        !transaction.strings().intern("V", value).ok()) return false;
    const enum_build_data fixed{enum_definition_state::opaque, false,
                                builtin_type::integer, {}};
    const enum_value_build values[] = {{value, {builtin_type::integer, 1}}};
    const enum_build_data non_fixed{enum_definition_state::defined, false,
                                    std::nullopt, values};
    stable_id id; type_handle type;
    auto a = open_source(transaction.graph_state(), source_id{1});
    auto b = open_source(transaction.graph_state(), source_id{2});
    if (!a.add_named_enum(name, fixed, id, type).ok() ||
        b.add_named_enum(name, non_fixed, id, type).ok()) return false;

    graph_manager invalid_manager;
    if (!invalid_manager.initialize().ok()) return false;
    auto invalid = invalid_manager.begin_build();
    if (!invalid.sources().add(source_a, project_item_role::source).ok() ||
        !invalid.strings().intern("Bad", name).ok() ||
        !invalid.strings().intern("BadValue", value).ok()) return false;
    const enum_value_build invalid_value[] = {
        {value, {builtin_type::unsigned_integer, 0x1'0000'0000ULL}}};
    const enum_build_data invalid_data{enum_definition_state::defined, false,
                                       std::nullopt, invalid_value};
    auto source = open_source(invalid.graph_state(), source_id{1});
    return !source.add_named_enum(name, invalid_data, id, type).ok();
}

bool test_anonymous_source_replacement()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    type_handle old;
    {
        auto transaction = manager.begin_build();
        if (!transaction.sources().add(source_a, project_item_role::source).ok()) return false;
        const enum_build_data data{enum_definition_state::defined, false,
                                   std::nullopt, {}};
        auto source = open_source(transaction.graph_state(), source_id{1});
        if (!source.add_anonymous_enum(data, old).ok() || !transaction.commit().ok()) return false;
    }
    if (manager.compiled_graph().find(old) == nullptr) return false;
    {
        auto transaction = manager.begin_build();
        auto source = open_source(transaction.graph_state(), source_id{1});
        (void)source;
        if (!transaction.commit().ok()) return false;
    }
    return manager.compiled_graph().find(old) == nullptr;
}

bool test_declaration_permutations()
{
    struct result
    {
        enum_type_record record{};
        source_id defining_source{};
        std::uint64_t value = 0;
    };
    const auto run = [](const std::array<int, 3>& order, result& output)
    {
        graph_manager manager;
        if (!manager.initialize().ok()) return false;
        auto transaction = manager.begin_build();
        if (!transaction.sources().add(source_a, project_item_role::source).ok() ||
            !transaction.sources().add(source_b, project_item_role::source).ok() ||
            !transaction.sources().add(source_c, project_item_role::source).ok()) return false;
        string_id name, value;
        if (!transaction.strings().intern("Permutation", name).ok() ||
            !transaction.strings().intern("P", value).ok()) return false;
        const enum_build_data opaque{enum_definition_state::opaque, false,
                                      builtin_type::integer, {}};
        const enum_value_build values[] = {{value, {builtin_type::integer, 9}}};
        const enum_build_data definition{enum_definition_state::defined, false,
                                          builtin_type::integer, values};
        stable_id id; type_handle type;
        for (const auto item : order)
        {
            auto source = open_source(transaction.graph_state(), source_id{static_cast<std::uint32_t>(item + 1)});
            if (!source.add_named_enum(name, item == 2 ? definition : opaque, id, type).ok())
                return false;
        }
        if (!transaction.commit().ok()) return false;
        const auto* entity = manager.compiled_graph().find(id);
        const auto* record = entity ? manager.compiled_graph().find(entity->type) : nullptr;
        const auto values_view = entity ? manager.compiled_graph().enum_values(entity->type)
                                        : std::span<const enum_value_record>{};
        if (!entity || !record || values_view.size() != 1) return false;
        output = {record->enumeration, entity->defining_source, values_view[0].bits};
        return true;
    };
    result a, b, c;
    if (!run({0, 1, 2}, a) || !run({2, 0, 1}, b) || !run({1, 2, 0}, c)) return false;
    const auto equal = [](const result& left, const result& right)
    {
        return left.record.definition_state == right.record.definition_state &&
               left.record.scoped == right.record.scoped &&
               left.record.fixed_underlying == right.record.fixed_underlying &&
               left.record.underlying == right.record.underlying &&
               left.record.enumerator_count == right.record.enumerator_count &&
               left.defining_source == right.defining_source && left.value == right.value;
    };
    return equal(a, b) && equal(b, c);
}

bool test_signed_widening_conversion()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    if (!transaction.sources().add(source_a, project_item_role::source).ok()) return false;
    string_id name, value;
    if (!transaction.strings().intern("Signed", name).ok() ||
        !transaction.strings().intern("MinusOne", value).ok()) return false;
    const enum_value_build values[] = {
        {value, {builtin_type::integer, 0xffffffffULL}}};
    const enum_build_data data{enum_definition_state::defined, false,
                               builtin_type::long_long_integer, values};
    stable_id id; type_handle type;
    auto source = open_source(transaction.graph_state(), source_id{1});
    if (!source.add_named_enum(name, data, id, type).ok() || !transaction.commit().ok())
        return false;
    const auto stored = manager.compiled_graph().enum_values(type);
    return stored.size() == 1 && stored[0].bits == 0xffffffffffffffffULL;
}

bool test_remove_reuse_resurrect_slot_ownership()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    string_id a_name, b_name;
    stable_id a_id; type_handle a_slot;
    const enum_build_data opaque{enum_definition_state::opaque, false,
                                  builtin_type::integer, {}};
    {
        auto transaction = manager.begin_build();
        if (!transaction.sources().add(source_a, project_item_role::source).ok() ||
            !transaction.sources().add(source_b, project_item_role::source).ok() ||
            !transaction.strings().intern("A", a_name).ok() ||
            !transaction.strings().intern("B", b_name).ok()) return false;
        auto source = open_source(transaction.graph_state(), source_id{1});
        if (!source.add_named_enum(a_name, opaque, a_id, a_slot).ok() ||
            !transaction.commit().ok()) return false;
    }
    stable_id b_id; type_handle b_slot, resurrected_slot;
    {
        auto transaction = manager.begin_build();
        auto a = open_source(transaction.graph_state(), source_id{1});
        auto b = open_source(transaction.graph_state(), source_id{2});
        if (!b.add_named_enum(b_name, opaque, b_id, b_slot).ok() ||
            !a.add_named_enum(a_name, opaque, a_id, resurrected_slot).ok() ||
            b_slot == a_slot || resurrected_slot != a_slot ||
            !transaction.commit().ok()) return false;
    }
    const auto* a = manager.compiled_graph().find(a_id);
    const auto* b = manager.compiled_graph().find(b_id);
    return a && b && a->type == a_slot && b->type == b_slot && a->type != b->type &&
           manager.compiled_graph().find(a->type) && manager.compiled_graph().find(b->type);
}

bool test_candidate_only_type_cancellation()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    if (!transaction.sources().add(source_a, project_item_role::source).ok()) return false;
    string_id name;
    if (!transaction.strings().intern("Transient", name).ok()) return false;
    const enum_build_data opaque{enum_definition_state::opaque, false,
                                  builtin_type::integer, {}};
    stable_id id; type_handle slot;
    auto source = open_source(transaction.graph_state(), source_id{1});
    if (!source.add_named_enum(name, opaque, id, slot).ok() ||
        !access::remove_candidate_entity(transaction.graph_state(), id).ok() ||
        transaction.graph_state().find(id) != nullptr ||
        !transaction.graph_state().enum_values(slot).empty() ||
        !transaction.commit().ok()) return false;
    return manager.compiled_graph().find(id) == nullptr &&
           manager.compiled_graph().find(slot) == nullptr &&
           manager.compiled_graph().user_type_count() == 0;
}

bool test_claimed_free_slot_cancellation()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    string_id old_name, candidate_name;
    stable_id old_id; type_handle free_slot;
    const enum_build_data opaque{enum_definition_state::opaque, false,
                                  builtin_type::integer, {}};
    {
        auto transaction = manager.begin_build();
        if (!transaction.sources().add(source_a, project_item_role::source).ok() ||
            !transaction.sources().add(source_b, project_item_role::source).ok() ||
            !transaction.strings().intern("Old", old_name).ok() ||
            !transaction.strings().intern("Candidate", candidate_name).ok()) return false;
        auto source = open_source(transaction.graph_state(), source_id{1});
        if (!source.add_named_enum(old_name, opaque, old_id, free_slot).ok() ||
            !transaction.commit().ok()) return false;
    }
    {
        auto transaction = manager.begin_build();
        auto source = open_source(transaction.graph_state(), source_id{1});
        (void)source;
        if (!transaction.commit().ok() || manager.compiled_graph().find(free_slot)) return false;
    }
    {
        auto transaction = manager.begin_build();
        stable_id candidate_id; type_handle claimed;
        auto source = open_source(transaction.graph_state(), source_id{2});
        if (!source.add_named_enum(candidate_name, opaque, candidate_id, claimed).ok() ||
            claimed != free_slot ||
            !access::remove_candidate_entity(transaction.graph_state(), candidate_id).ok() ||
            !transaction.commit().ok()) return false;
    }
    if (manager.compiled_graph().find(free_slot) != nullptr ||
        manager.compiled_graph().user_type_count() != 0) return false;
    {
        auto transaction = manager.begin_build();
        const enum_build_data anonymous{enum_definition_state::defined, false,
                                         std::nullopt, {}};
        type_handle reused;
        auto source = open_source(transaction.graph_state(), source_id{2});
        if (!source.add_anonymous_enum(anonymous, reused).ok() || reused != free_slot ||
            !transaction.commit().ok()) return false;
    }
    return manager.compiled_graph().find(free_slot) != nullptr &&
           manager.compiled_graph().user_type_count() == 1;
}

bool test_reversible_named_type_contributions()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    string_id name;
    stable_id identity;

    // Definition first, declaration second: one canonical identity.
    {
        auto transaction = manager.begin_build();
        source_id definition_source, declaration_source;
        if (!transaction.sources().resolve(source_b, project_item_role::source,
                                           definition_source).ok() ||
            !transaction.sources().resolve(source_a, project_item_role::source,
                                           declaration_source).ok() ||
            !transaction.strings().intern("B", name).ok()) return false;
        type_handle definition_type, declaration_type;
        auto definition = open_source(transaction.graph_state(), definition_source);
        if (!definition.add_named_type(name, aggregate_definition_state::defined,
                                       identity, definition_type).ok()) return false;
        stable_id declaration_identity;
        auto declaration = open_source(transaction.graph_state(), declaration_source);
        if (!declaration.add_named_type(name, aggregate_definition_state::declared,
                                        declaration_identity, declaration_type).ok() ||
            declaration_identity != identity || declaration_type != definition_type ||
            !transaction.commit().ok()) return false;
    }
    const auto* entity = manager.compiled_graph().find(name);
    const auto* type = entity ? manager.compiled_graph().find(entity->type) : nullptr;
    if (!entity || entity->id != identity || entity->kind != entity_kind::aggregate_type ||
        entity->defining_source != source_id{1} || !type ||
        type->kind != user_type_kind::aggregate ||
        type->aggregate.definition_state != aggregate_definition_state::defined)
        return false;

    // Remove only the declaration: the definition and identity remain.
    {
        auto transaction = manager.begin_build();
        source_id source;
        if (!transaction.sources().resolve(source_a, project_item_role::source, source).ok())
            return false;
        auto replacement = open_source(transaction.graph_state(), source);
        if (!transaction.commit().ok()) return false;
    }
    entity = manager.compiled_graph().find(name);
    if (!entity || entity->id != identity || entity->defining_source != source_id{1})
        return false;

    // Replace the defining Source with a valid definition: no identity transition.
    {
        auto transaction = manager.begin_build();
        source_id source;
        string_id same_name;
        if (!transaction.sources().resolve(source_b, project_item_role::source, source).ok() ||
            !transaction.strings().intern("B", same_name).ok()) return false;
        stable_id same_identity;
        type_handle same_type;
        auto replacement = open_source(transaction.graph_state(), source);
        if (!replacement.add_named_type(same_name, aggregate_definition_state::defined,
                                        same_identity, same_type).ok() ||
            same_identity != identity || !transaction.commit().ok()) return false;
    }

    // Add a declaration, then remove the definition: defined -> declared, same ID.
    {
        auto transaction = manager.begin_build();
        source_id source;
        string_id same_name;
        if (!transaction.sources().resolve(source_a, project_item_role::source, source).ok() ||
            !transaction.strings().intern("B", same_name).ok()) return false;
        stable_id same_identity;
        type_handle ignored;
        auto replacement = open_source(transaction.graph_state(), source);
        if (!replacement.add_named_type(same_name, aggregate_definition_state::declared,
                                        same_identity, ignored).ok() ||
            same_identity != identity || !transaction.commit().ok()) return false;
    }
    {
        auto transaction = manager.begin_build();
        source_id source;
        if (!transaction.sources().resolve(source_b, project_item_role::source, source).ok())
            return false;
        auto replacement = open_source(transaction.graph_state(), source);
        if (!transaction.commit().ok()) return false;
    }
    entity = manager.compiled_graph().find(name);
    type = entity ? manager.compiled_graph().find(entity->type) : nullptr;
    if (!entity || entity->id != identity || entity->defining_source || !type ||
        type->aggregate.definition_state != aggregate_definition_state::declared)
        return false;

    // Same Source may contribute declaration + definition.
    {
        auto transaction = manager.begin_build();
        source_id source;
        string_id same_name;
        if (!transaction.sources().resolve(source_a, project_item_role::source, source).ok() ||
            !transaction.strings().intern("B", same_name).ok()) return false;
        stable_id first, second;
        type_handle first_type, second_type;
        auto replacement = open_source(transaction.graph_state(), source);
        if (!replacement.add_named_type(same_name, aggregate_definition_state::declared,
                                        first, first_type).ok() ||
            !replacement.add_named_type(same_name, aggregate_definition_state::defined,
                                        second, second_type).ok() ||
            first != identity || second != identity || !transaction.commit().ok())
            return false;
    }
    entity = manager.compiled_graph().find(name);
    if (!entity || entity->id != identity || !entity->defining_source) return false;

    // A second live definition fails atomically and preserves committed G.
    {
        auto transaction = manager.begin_build();
        source_id source;
        string_id same_name;
        if (!transaction.sources().resolve(source_c, project_item_role::source, source).ok() ||
            !transaction.strings().intern("B", same_name).ok()) return false;
        stable_id rejected;
        type_handle rejected_type;
        auto replacement = open_source(transaction.graph_state(), source);
        if (replacement.add_named_type(same_name, aggregate_definition_state::defined,
                                       rejected, rejected_type).ok() ||
            transaction.commit().ok()) return false;
    }
    entity = manager.compiled_graph().find(name);
    if (!entity || entity->id != identity || entity->defining_source != source_id{2})
        return false;

    // Removing the final contribution removes the canonical entity.
    {
        auto transaction = manager.begin_build();
        source_id source;
        if (!transaction.sources().resolve(source_a, project_item_role::source, source).ok())
            return false;
        auto replacement = open_source(transaction.graph_state(), source);
        if (!transaction.commit().ok()) return false;
    }
    if (manager.compiled_graph().find(name) != nullptr) return false;

    // Enum and aggregate declarations cannot share one canonical type identity.
    {
        auto transaction = manager.begin_build();
        source_id source;
        string_id same_name;
        if (!transaction.sources().resolve(source_a, project_item_role::source, source).ok() ||
            !transaction.strings().intern("B", same_name).ok()) return false;
        stable_id aggregate_id, enum_id;
        type_handle aggregate_type, enum_type;
        auto replacement = open_source(transaction.graph_state(), source);
        const enum_build_data opaque_enum{enum_definition_state::opaque, true,
                                          std::nullopt, {}};
        if (!replacement.add_named_type(same_name, aggregate_definition_state::declared,
                                        aggregate_id, aggregate_type).ok() ||
            replacement.add_named_enum(same_name, opaque_enum,
                                       enum_id, enum_type).ok() ||
            transaction.commit().ok()) return false;
    }
    return manager.compiled_graph().find(name) == nullptr;
}

bool test_compiled_checkpoint_roundtrip_and_corruption()
{
    graph_manager saved;
    if (!saved.initialize({abi_target::windows_x64,16}).ok()) return false;
    auto transaction = saved.begin_build();
    source_id source, retired_source;
    string_id name, value_name, retired_name;
    if (!transaction.sources().resolve(std::filesystem::path{L"C:\\compiled\\a.hpp"}, project_item_role::source, source).ok() ||
        !transaction.sources().resolve(std::filesystem::path{L"C:\\compiled\\retired.hpp"}, project_item_role::source, retired_source).ok() ||
        !transaction.strings().intern("N::Persisted", name).ok() ||
        !transaction.strings().intern("Answer", value_name).ok() ||
        !transaction.strings().intern("N::Retired", retired_name).ok()) return false;
    const std::array values{enum_value_build{value_name,{builtin_type::unsigned_long_long_integer,42}}};
    const enum_build_data data{enum_definition_state::defined,true,builtin_type::unsigned_long_long_integer,values};
    stable_id id; type_handle type;
    auto replacement=open_source(transaction.graph_state(),source);
    stable_id retired_id;type_handle retired_type;auto retired_replacement=open_source(transaction.graph_state(),retired_source);const enum_build_data opaque{enum_definition_state::opaque,true,builtin_type::integer,{}};
    if(!replacement.add_named_enum(name,data,id,type).ok()||!retired_replacement.add_named_enum(retired_name,opaque,retired_id,retired_type).ok()||!transaction.commit().ok())return false;
    {auto removal=saved.begin_build();source_id same;if(!removal.sources().resolve(std::filesystem::path{L"C:\\compiled\\retired.hpp"},project_item_role::source,same).ok()||same!=retired_source)return false;auto empty=open_source(removal.graph_state(),same);if(!removal.commit().ok())return false;}
    const auto file=std::filesystem::temp_directory_path()/L"cw_compiled_roundtrip.bin";
    std::error_code ignored;std::filesystem::remove(file,ignored);
    const auto save_result=saved.save_compiled_checkpoint(file);if(!save_result.ok()){std::cerr<<"save code "<<static_cast<unsigned>(save_result.code)<<'\n';return false;}
    const auto failed_save=saved.save_compiled_checkpoint(file/L"missing"/L"compiled.bin");const graph* still_runnable=nullptr;
    if(failed_save.ok()||saved.state()!=project_state::valid||!saved.runnable_graph(still_runnable).ok()||still_runnable!=&saved.compiled_graph())return false;
    graph_manager loaded;const auto init_result=loaded.initialize();const auto load_result=init_result.ok()?loaded.load_compiled_checkpoint(file):init_result;if(!load_result.ok()){std::cerr<<"load code "<<static_cast<unsigned>(load_result.code)<<'\n';return false;}
    const auto loaded_name=loaded.strings().find("N::Persisted");const auto loaded_value=loaded.strings().find("Answer");const auto loaded_retired=loaded.strings().find("N::Retired");
    const auto* entity=loaded.compiled_graph().find(loaded_name);const auto enum_values=entity?loaded.compiled_graph().enum_values(entity->type):std::span<const enum_value_record>{};
    if(loaded.state()!=project_state::valid||loaded.compiled_graph().abi().pack!=16||loaded_name!=name||loaded_value!=value_name||loaded_retired!=retired_name||loaded.compiled_graph().find(loaded_retired)!=nullptr||loaded.compiled_graph().find(retired_id)!=nullptr||loaded.compiled_graph().find(retired_type)!=nullptr||!entity||entity->id!=id||entity->type!=type||entity->defining_source!=source||enum_values.size()!=1||enum_values[0].name!=value_name||enum_values[0].bits!=42){std::cerr<<"semantic mismatch\n";return false;}
    {std::fstream corrupt(file,std::ios::binary|std::ios::in|std::ios::out);char bad='X';corrupt.write(&bad,1);}
    graph_manager rejected;if(!rejected.initialize().ok()||rejected.load_compiled_checkpoint(file).ok()||rejected.state()!=project_state::error)return false;
    const graph* runnable=nullptr;if(rejected.runnable_graph(runnable).ok()||runnable!=nullptr)return false;
    std::filesystem::remove(file,ignored);return true;
}

bool test_canonical_derived_type_core()
{
    graph_manager manager;if(!manager.initialize().ok())return false;
    auto tx=manager.begin_build();if(!tx.sources().add(source_a,project_item_role::source).ok())return false;
    graph_update::source_replacement r;if(!tx.graph_state().replace_source(source_id{1},r).ok())return false;
    const auto integer=r.builtin_type_ref(builtin_type::integer);
    const auto void_type=r.builtin_type_ref(builtin_type::void_type);
    if(!integer||!void_type||integer==void_type||TypeRef{}==void_type)return false;
    TypeRef pointer,pointer_again,pointer_pointer,array4,array8,array_pointer4,pointer_array4,lref_array4,array_array;
    if(!r.get_or_create_pointer(integer,pointer).ok()||
       !r.get_or_create_pointer(integer,pointer_again).ok()||pointer!=pointer_again||
       !r.get_or_create_pointer(pointer,pointer_pointer).ok()||
       !r.get_or_create_array(integer,4,array4).ok()||
       !r.get_or_create_array(integer,8,array8).ok()||array4==array8||
       !r.get_or_create_array(pointer,4,array_pointer4).ok()||
       !r.get_or_create_pointer(array4,pointer_array4).ok()||
       !r.get_or_create_lvalue_reference(array4,lref_array4).ok()||
       !r.get_or_create_array(array8,4,array_array).ok()||
       array_pointer4==pointer_array4||array_pointer4==lref_array4||pointer_array4==lref_array4)
        return false;
    if(!tx.commit().ok()||manager.compiled_graph().derived_type_count()!=8)return false;
    const auto* ap=manager.compiled_graph().derived(array_pointer4);
    const auto* pa=manager.compiled_graph().derived(pointer_array4);
    const auto* ra=manager.compiled_graph().derived(lref_array4);
    if(!ap||!pa||!ra||ap->kind!=derived_type_kind::array||ap->child!=pointer||ap->payload!=4||
       pa->kind!=derived_type_kind::pointer||pa->child!=array4||pa->payload!=0||
       ra->kind!=derived_type_kind::lvalue_reference||ra->child!=array4)return false;
    const auto before=manager.compiled_graph().derived_type_count();
    auto failed=manager.begin_build();graph_update::source_replacement failed_r;
    if(!failed.graph_state().replace_source(source_id{1},failed_r).ok())return false;
    TypeRef candidate,invalid;
    if(!failed_r.get_or_create_rvalue_reference(integer,candidate).ok()||
       failed_r.get_or_create_array(integer,0,invalid).ok()||failed.commit().ok())return false;
    return manager.compiled_graph().derived_type_count()==before&&manager.compiled_graph().derived(candidate)==nullptr;
}
} // namespace

int main()
{
    const struct { const char* name; bool (*run)(); } tests[] = {
        {"project lifecycle", test_project_lifecycle_runtime_gate},
        {"discard/prepared", test_discard_and_prepared_mutation_rejection},
        {"success/identity", test_success_identity_and_second_commit},
        {"prepare failures", test_forced_prepare_failures_are_atomic},
        {"stale generations", test_stale_generations_are_atomic},
        {"move/local commits", test_move_and_local_commits},
        {"type/enum core", test_type_values_and_enum_core},
        {"enum transitions/ABI", test_enum_transitions_identity_and_abi},
        {"enum failures/nonreuse", test_enum_redeclaration_failures_and_nonreuse},
        {"stale graph", test_stale_graph_and_graph_prepare_atomicity},
        {"canonical contributions", test_canonical_contribution_recomposition},
        {"fixed underlying/constants", test_fixed_underlying_and_constant_boundary},
        {"anonymous replacement", test_anonymous_source_replacement},
        {"declaration permutations", test_declaration_permutations},
        {"signed widening", test_signed_widening_conversion},
        {"slot ownership", test_remove_reuse_resurrect_slot_ownership},
        {"candidate-only cancellation", test_candidate_only_type_cancellation},
        {"claimed-free cancellation", test_claimed_free_slot_cancellation},
        {"reversible named types", test_reversible_named_type_contributions},
        {"compiled checkpoint", test_compiled_checkpoint_roundtrip_and_corruption},
        {"canonical derived types", test_canonical_derived_type_core}};
    for (const auto& test : tests)
        if (!test.run()) { std::cerr << "FAILED: " << test.name << '\n'; return 1; }
    return 0;
}
