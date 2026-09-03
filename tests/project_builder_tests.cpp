#include "../server_entry/project/builder/project_builder.hpp"
#include "../server_entry/project/graph/graph_manager.hpp"
#include "../server_entry/project/parser/parser.hpp"
#include "../server_entry/project/parser/lexer.hpp"
#include "../server_entry/project/construction/source_publisher.hpp"
#include "../server_entry/project/construction/source_frontend_generation.hpp"
#include "../server_entry/metrics/source_acquisition_telemetry.hpp"
#include "../server_entry/diagnostics/diagnostic_descriptor.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <span>
#include <type_traits>

namespace cw::server
{
class graph_build_transaction_test_access
{
public:
    static std::uint64_t graph_generation(const graph_manager& manager) noexcept
    {
        return manager.graph_.generation_;
    }
};
} // namespace cw::server

namespace
{
using namespace cw::server;
using access = graph_build_transaction_test_access;

static_assert(std::is_empty_v<project_builder>);
static_assert(sizeof(project_builder) == 1);
static_assert(!std::is_move_constructible_v<source_environment_storage>);
static_assert(!std::is_move_assignable_v<source_environment_storage>);
static_assert(sizeof(source_constant_binding) == 48);
static_assert(sizeof(source_type_binding) == 48);
static_assert(source_environment_storage::lookup_key_size() == 32);
static_assert(sizeof(aggregate_declaration_source_fact) <= 48);
static_assert(std::is_same_v<
    decltype(std::declval<const source_frontend_generation&>().interface(source_id{})),
    const source_environment_storage*>);
static_assert(std::is_base_of_v<parser_backend, native_parser_backend>);

constexpr auto integer = builtin_type::integer;

bool add_source(graph_build_transaction& transaction, const wchar_t* path)
{
    return transaction.sources().add(
        std::filesystem::path{path}, project_item_role::source).ok();
}

bool intern(graph_build_transaction& transaction, const char* value, string_id& id)
{
    return transaction.strings().intern(value, id).ok();
}

bool initialize_sources(graph_manager& manager, const std::size_t count)
{
    auto transaction = manager.begin_build();
    const std::array paths{
        LR"(C:\builder\a.cpp)", LR"(C:\builder\b.cpp)",
        LR"(C:\builder\c.cpp)"};
    for (std::size_t index = 0; index < count; ++index)
        if (!add_source(transaction, paths[index])) return false;
    return transaction.commit().ok();
}

enum_source_fact named(string_id name, enum_definition_state state,
                       std::span<const enum_value_fact> values = {})
{
    return {name, false, false, state, integer, values};
}

enum_source_fact anonymous(std::span<const enum_value_fact> values)
{
    return {{}, true, false, enum_definition_state::defined, integer, values};
}

status run_builder(project_builder& builder, graph_build_transaction& transaction,
                   std::span<const source_fact_batch> batches,
                   diagnostic_buffer* output = nullptr)
{
    diagnostic_buffer local;
    return builder.build(transaction, batches, operation_id{701},
                         output ? *output : local);
}

bool apply(graph_manager& manager, std::span<const source_fact_batch> batches)
{
    auto transaction = manager.begin_build();
    project_builder builder;
    return run_builder(builder, transaction, batches).ok() && transaction.commit().ok();
}

bool test_named_definition()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    auto transaction = manager.begin_build();
    string_id e, a, b;
    if (!intern(transaction, "E", e) || !intern(transaction, "A", a) ||
        !intern(transaction, "B", b)) return false;
    const enum_value_fact values[] = {{a, {integer, 0}}, {b, {integer, 1}}};
    const enum_source_fact facts[] = {named(e, enum_definition_state::defined, values)};
    const source_fact_batch batches[] = {{source_id{1}, facts}};
    project_builder builder;
    if (!run_builder(builder, transaction, batches).ok() || !transaction.commit().ok()) return false;
    const auto* entity = manager.compiled_graph().find(e);
    if (!entity || !entity->id || !entity->type || entity->defining_source != source_id{1}) return false;
    const auto* type = manager.compiled_graph().find(entity->type);
    const auto stored = manager.compiled_graph().enum_values(entity->type);
    return type && type->enumeration.definition_state == enum_definition_state::defined &&
           stored.size() == 2 && stored[0].name == a && stored[0].bits == 0 &&
           stored[1].name == b && stored[1].bits == 1;
}

bool test_opaque_definition_and_replacement()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 2)) return false;
    string_id e, a;
    {
        auto transaction = manager.begin_build();
        if (!intern(transaction, "E", e) || !intern(transaction, "A", a)) return false;
        const enum_source_fact s1[] = {named(e, enum_definition_state::opaque)};
        const enum_value_fact values[] = {{a, {integer, 0}}};
        const enum_source_fact s2[] = {named(e, enum_definition_state::defined, values)};
        const source_fact_batch batches[] = {{source_id{1}, s1}, {source_id{2}, s2}};
        project_builder builder;
        if (!run_builder(builder, transaction, batches).ok() || !transaction.commit().ok()) return false;
    }
    const auto original = manager.compiled_graph().find(e)->id;
    const source_fact_batch remove_definition[] = {{source_id{2}, {}}};
    if (!apply(manager, remove_definition)) return false;
    const auto* entity = manager.compiled_graph().find(e);
    const auto* type = entity ? manager.compiled_graph().find(entity->type) : nullptr;
    return entity && entity->id == original && !entity->defining_source && type &&
           type->enumeration.definition_state == enum_definition_state::opaque;
}

bool test_repeated_opaque_and_incompatible_atomicity()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 2)) return false;
    string_id e;
    {
        auto transaction = manager.begin_build();
        if (!intern(transaction, "E", e)) return false;
        const enum_source_fact repeated[] = {
            named(e, enum_definition_state::opaque),
            named(e, enum_definition_state::opaque)};
        const source_fact_batch batch[] = {{source_id{1}, repeated}};
        project_builder builder;
        if (!run_builder(builder, transaction, batch).ok() || !transaction.commit().ok()) return false;
    }
    const auto before = manager.compiled_graph().find(e)->id;
    auto transaction = manager.begin_build();
    enum_source_fact incompatible = named(e, enum_definition_state::opaque);
    incompatible.explicit_underlying = builtin_type::short_integer;
    const source_fact_batch batch[] = {{source_id{2}, {&incompatible, 1}}};
    project_builder builder;
    if (run_builder(builder, transaction, batch).ok() || transaction.commit().ok()) return false;
    const auto* after = manager.compiled_graph().find(e);
    return after && after->id == before && manager.compiled_graph().contribution_count(source_id{2}) == 0;
}

bool test_duplicate_definition_atomicity()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 2)) return false;
    auto transaction = manager.begin_build();
    string_id e, a, b;
    if (!intern(transaction, "E", e) || !intern(transaction, "A", a) ||
        !intern(transaction, "B", b)) return false;
    const enum_value_fact av[] = {{a, {integer, 0}}};
    const enum_value_fact bv[] = {{b, {integer, 1}}};
    const enum_source_fact af[] = {named(e, enum_definition_state::defined, av)};
    const enum_source_fact bf[] = {named(e, enum_definition_state::defined, bv)};
    const source_fact_batch batches[] = {{source_id{1}, af}, {source_id{2}, bf}};
    project_builder builder;
    return !run_builder(builder, transaction, batches).ok() && !transaction.commit().ok() &&
           manager.compiled_graph().entity_count() == 0;
}

bool test_remove_final_and_no_id_reuse()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    string_id e, f;
    {
        auto transaction = manager.begin_build();
        if (!intern(transaction, "E", e)) return false;
        const enum_source_fact facts[] = {named(e, enum_definition_state::opaque)};
        const source_fact_batch batch[] = {{source_id{1}, facts}};
        project_builder builder;
        if (!run_builder(builder, transaction, batch).ok() || !transaction.commit().ok()) return false;
    }
    const auto old = manager.compiled_graph().find(e)->id;
    const source_fact_batch empty[] = {{source_id{1}, {}}};
    if (!apply(manager, empty) || manager.compiled_graph().find(e) ||
        manager.compiled_graph().find(old)) return false;
    auto transaction = manager.begin_build();
    if (!intern(transaction, "F", f)) return false;
    const enum_source_fact facts[] = {named(f, enum_definition_state::opaque)};
    const source_fact_batch batch[] = {{source_id{1}, facts}};
    project_builder builder;
    if (!run_builder(builder, transaction, batch).ok() || !transaction.commit().ok()) return false;
    return manager.compiled_graph().find(f)->id != old;
}

bool test_move_definition_and_omitted_source()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 3)) return false;
    string_id e, f, value;
    {
        auto transaction = manager.begin_build();
        if (!intern(transaction, "E", e) || !intern(transaction, "F", f) ||
            !intern(transaction, "V", value)) return false;
        const enum_value_fact values[] = {{value, {integer, 0}}};
        const enum_source_fact s1[] = {named(e, enum_definition_state::defined, values)};
        const enum_source_fact s2[] = {named(e, enum_definition_state::opaque)};
        const enum_source_fact s3[] = {named(f, enum_definition_state::opaque)};
        const source_fact_batch batches[] = {
            {source_id{1}, s1}, {source_id{2}, s2}, {source_id{3}, s3}};
        project_builder builder;
        if (!run_builder(builder, transaction, batches).ok() || !transaction.commit().ok()) return false;
    }
    const auto e_id = manager.compiled_graph().find(e)->id;
    const auto f_id = manager.compiled_graph().find(f)->id;
    const enum_value_fact values[] = {{value, {integer, 0}}};
    const source_fact_batch clear_s1[] = {{source_id{1}, {}}};
    if (!apply(manager, clear_s1)) return false;
    const enum_source_fact definition[] = {named(e, enum_definition_state::defined, values)};
    const source_fact_batch define_s2[] = {{source_id{2}, definition}};
    if (!apply(manager, define_s2)) return false;
    const auto* moved = manager.compiled_graph().find(e);
    const auto* omitted = manager.compiled_graph().find(f);
    return moved && moved->id == e_id && moved->defining_source == source_id{2} &&
           omitted && omitted->id == f_id;
}

bool test_anonymous_lifetime()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    {
        auto transaction = manager.begin_build();
        string_id value;
        if (!intern(transaction, "A", value)) return false;
        const enum_value_fact values[] = {{value, {integer, 0}}};
        const enum_source_fact facts[] = {anonymous(values)};
        const source_fact_batch batch[] = {{source_id{1}, facts}};
        project_builder builder;
        if (!run_builder(builder, transaction, batch).ok() || !transaction.commit().ok()) return false;
    }
    if (manager.compiled_graph().user_type_count() != 1 ||
        manager.compiled_graph().entity_count() != 0) return false;
    const source_fact_batch empty[] = {{source_id{1}, {}}};
    return apply(manager, empty) && manager.compiled_graph().user_type_count() == 0;
}

bool test_structural_validation_aborts_transaction()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    auto transaction = manager.begin_build();
    string_id e;
    if (!intern(transaction, "E", e)) return false;
    enum_source_fact facts[] = {
        named(e, enum_definition_state::opaque),
        {e, true, false, enum_definition_state::opaque, integer, {}}};
    const source_fact_batch batch[] = {{source_id{1}, facts}};
    project_builder builder;
    return !run_builder(builder, transaction, batch).ok() && !transaction.commit().ok() &&
           manager.compiled_graph().entity_count() == 0;
}

bool test_duplicate_source_batch_cannot_publish_merged_state()
{
    for (const auto [first_empty, second_empty] :
         {std::pair{false, false}, std::pair{false, true},
          std::pair{true, false}, std::pair{true, true}})
    {
        graph_manager manager;
        if (!manager.initialize().ok() || !initialize_sources(manager, 2)) return false;
        const auto generation = access::graph_generation(manager);
        auto transaction = manager.begin_build();
        string_id e1, e2, later;
        if (!intern(transaction, "E1", e1) || !intern(transaction, "E2", e2) ||
            !intern(transaction, "Later", later)) return false;
        const enum_source_fact first_fact[] = {named(e1, enum_definition_state::opaque)};
        const enum_source_fact second_fact[] = {named(e2, enum_definition_state::opaque)};
        const enum_source_fact later_fact[] = {named(later, enum_definition_state::opaque)};
        const source_fact_batch batches[] = {
            {source_id{1}, first_empty ? std::span<const enum_source_fact>{} : first_fact},
            {source_id{1}, second_empty ? std::span<const enum_source_fact>{} : second_fact},
            {source_id{2}, later_fact}};
        project_builder builder;
        diagnostic_buffer emitted;
        if (run_builder(builder, transaction, batches, &emitted).code !=
                status_code::duplicate_source_replacement ||
            transaction.commit().ok() || manager.compiled_graph().entity_count() != 0 ||
            manager.compiled_graph().contribution_count(source_id{1}) != 0 ||
            manager.compiled_graph().contribution_count(source_id{2}) != 0 ||
            access::graph_generation(manager) != generation ||
            emitted.records().size() != 1 ||
            emitted.records()[0].id !=
                diagnostics::builder_duplicate_source_replacement.id ||
            emitted.records()[0].location.source != source_id{1} ||
            emitted.records()[0].operation != operation_id{701})
            return false;

        // A failed update does not poison a fresh transaction.
        auto fresh = manager.begin_build();
        string_id fresh_name;
        if (!intern(fresh, "Fresh", fresh_name)) return false;
        const enum_source_fact fresh_fact[] = {named(fresh_name, enum_definition_state::opaque)};
        const source_fact_batch valid[] = {{source_id{1}, fresh_fact}};
        if (!run_builder(builder, fresh, valid).ok() || !fresh.commit().ok() ||
            manager.compiled_graph().contribution_count(source_id{1}) != 1)
            return false;
    }
    return true;
}

bool test_replace_source_reports_duplicate_at_open_boundary()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    auto transaction = manager.begin_build();
    graph_update::source_replacement first;
    graph_update::source_replacement duplicate;
    if (!transaction.graph_state().replace_source(source_id{1}, first).ok()) return false;
    if (transaction.graph_state().replace_source(source_id{1}, duplicate).code !=
        status_code::duplicate_source_replacement) return false;
    type_handle unusable;
    const enum_build_data data{enum_definition_state::defined, false,
                               builtin_type::integer, {}};
    return duplicate.add_anonymous_enum(data, unusable).code == status_code::invalid_state &&
           !transaction.commit().ok();
}

bool test_permutation_of_existing_identity()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 2)) return false;
    string_id e;
    {
        auto transaction = manager.begin_build();
        if (!intern(transaction, "E", e)) return false;
        const enum_source_fact fact[] = {named(e, enum_definition_state::opaque)};
        const source_fact_batch batch[] = {{source_id{1}, fact}};
        project_builder builder;
        if (!run_builder(builder, transaction, batch).ok() || !transaction.commit().ok()) return false;
    }
    const auto id = manager.compiled_graph().find(e)->id;
    const enum_source_fact fact[] = {named(e, enum_definition_state::opaque)};
    const source_fact_batch reversed[] = {{source_id{2}, fact}, {source_id{1}, fact}};
    if (!apply(manager, reversed)) return false;
    return manager.compiled_graph().find(e)->id == id &&
           manager.compiled_graph().contribution_count(source_id{1}) == 1 &&
           manager.compiled_graph().contribution_count(source_id{2}) == 1;
}

bool test_supplied_source_and_declaration_order_is_preserved()
{
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    {
        auto transaction = manager.begin_build();
        for (std::uint32_t index = 1; index <= 30; ++index)
        {
            const auto path = std::filesystem::path{LR"(C:\builder\ordered)"} /
                              (std::to_wstring(index) + L".cpp");
            if (!transaction.sources().add(path, project_item_role::source).ok()) return false;
        }
        if (!transaction.commit().ok()) return false;
    }

    auto transaction = manager.begin_build();
    string_id a, b, z, lexical_a;
    if (!intern(transaction, "SourceA", a) || !intern(transaction, "SourceB", b) ||
        !intern(transaction, "Z", z) || !intern(transaction, "A", lexical_a)) return false;
    const enum_source_fact source_30[] = {
        named(a, enum_definition_state::opaque),
        named(z, enum_definition_state::opaque),
        named(lexical_a, enum_definition_state::opaque)};
    const enum_source_fact source_10[] = {named(b, enum_definition_state::opaque)};
    const source_fact_batch batches[] = {
        {source_id{30}, source_30},
        {source_id{10}, source_10}};
    project_builder builder;
    if (!run_builder(builder, transaction, batches).ok() || !transaction.commit().ok()) return false;

    const auto* first = manager.compiled_graph().find(a);
    const auto* second = manager.compiled_graph().find(z);
    const auto* third = manager.compiled_graph().find(lexical_a);
    const auto* fourth = manager.compiled_graph().find(b);
    return first && second && third && fourth &&
           first->id.value() < second->id.value() &&
           second->id.value() < third->id.value() &&
           third->id.value() < fourth->id.value();
}

bool test_worker_completion_order_is_resolved_before_builder()
{
    const auto build_after_completion = [](const std::array<int, 2>& completion_order)
    {
        // Completion order belongs to the coordinator and deliberately does not
        // participate in the canonical span passed to Builder.
        volatile int observed = completion_order[0] + completion_order[1];
        (void)observed;

        graph_manager manager;
        if (!manager.initialize().ok() || !initialize_sources(manager, 2))
            return std::array<std::uint32_t, 2>{};
        auto transaction = manager.begin_build();
        string_id first_name, second_name;
        if (!intern(transaction, "First", first_name) ||
            !intern(transaction, "Second", second_name))
            return std::array<std::uint32_t, 2>{};
        const enum_source_fact first[] = {named(first_name, enum_definition_state::opaque)};
        const enum_source_fact second[] = {named(second_name, enum_definition_state::opaque)};
        const source_fact_batch canonical[] = {
            {source_id{1}, first},
            {source_id{2}, second}};
        project_builder builder;
        if (!run_builder(builder, transaction, canonical).ok() || !transaction.commit().ok())
            return std::array<std::uint32_t, 2>{};
        return std::array{
            manager.compiled_graph().find(first_name)->id.value(),
            manager.compiled_graph().find(second_name)->id.value()};
    };

    const auto forward_completion = build_after_completion({0, 1});
    const auto reverse_completion = build_after_completion({1, 0});
    return forward_completion[0] != 0 && forward_completion == reverse_completion;
}

status run_parser_builder(project_builder& builder,
                          graph_build_transaction& transaction,
                          std::span<const parser_source_fact_batch> batches,
                          diagnostic_buffer& diagnostics)
{
    for (const auto& batch : batches)
    {
        const auto result = publish_source_facts(
            transaction, batch, builder, operation_id{702}, diagnostics);
        if (!result.ok()) return result;
    }
    return {};
}

bool test_parser_facts_are_canonicalized_and_detached_before_publication()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;

    constexpr std::string_view text =
        "namespace N { enum class E : int { A = 0, B = 1 }; }";
    source_context context;
    if (!parse_source({source_id{1}, text}, {}, operation_id{702}, context).ok())
        return false;

    const parser_source_fact_batch batches[] = {
        {source_id{1}, &context, context.enums}};
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    if (!run_parser_builder(builder, transaction, batches, diagnostics).ok()) return false;

    // Parser storage is transient. Canonicalization must have copied/interned
    // everything required by the transaction before publication.
    context.reset();
    if (!transaction.commit().ok()) return false;

    const auto enum_name = manager.strings().find("N::E");
    const auto a = manager.strings().find("A");
    const auto b = manager.strings().find("B");
    const auto* entity = manager.compiled_graph().find(enum_name);
    if (!enum_name || !a || !b || !entity || entity->defining_source != source_id{1})
        return false;
    const auto values = manager.compiled_graph().enum_values(entity->type);
    return values.size() == 2 && values[0].name == a && values[0].bits == 0 &&
           values[1].name == b && values[1].bits == 1;
}

bool test_parser_worker_completion_does_not_change_canonical_ids()
{
    const auto build = [](const bool reverse)
    {
        graph_manager manager;
        if (!manager.initialize().ok() || !initialize_sources(manager, 2))
            return std::array<std::uint32_t, 4>{};
        source_context first;
        source_context second;
        constexpr std::string_view first_text = "enum First { A = 1 };";
        constexpr std::string_view second_text = "enum Second { B = 2 };";
        const auto parse_first = [&]
        {
            return parse_source(
                {source_id{1}, first_text}, {}, operation_id{703}, first).ok();
        };
        const auto parse_second = [&]
        {
            return parse_source(
                {source_id{2}, second_text}, {}, operation_id{703}, second).ok();
        };
        if (reverse ? (!parse_second() || !parse_first())
                    : (!parse_first() || !parse_second()))
            return std::array<std::uint32_t, 4>{};

        // Coordinator order, not worker completion order, is authoritative.
        const parser_source_fact_batch canonical[] = {
            {source_id{1}, &first, first.enums},
            {source_id{2}, &second, second.enums}};
        auto transaction = manager.begin_build();
        project_builder builder;
        diagnostic_buffer diagnostics;
        if (!run_parser_builder(builder, transaction, canonical, diagnostics).ok() ||
            !transaction.commit().ok())
            return std::array<std::uint32_t, 4>{};
        const auto first_id = manager.strings().find("First");
        const auto second_id = manager.strings().find("Second");
        return std::array{
            first_id.value(), second_id.value(),
            manager.compiled_graph().find(first_id)->id.value(),
            manager.compiled_graph().find(second_id)->id.value()};
    };

    const auto forward = build(false);
    const auto reverse = build(true);
    return forward[0] != 0 && forward == reverse;
}

bool test_parser_fail_fast_and_preserves_committed_graph()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    source_context context;
    constexpr std::string_view invalid =
        "enum Broken { A = ? }; enum MustNotParse { B = 1 };";
    const auto parsed = parse_source(
        {source_id{1}, invalid}, {}, operation_id{704}, context);
    return !parsed.ok() && context.diagnostics.records().size() == 1 &&
           context.enums.empty() && manager.compiled_graph().entity_count() == 0;
}

bool test_source_name_refs_survive_context_growth()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    std::string text;
    for (int index = 0; index < 4000; ++index)
        text += "enum Growth" + std::to_string(index) + " : int;\n";
    source_context context;
    if (!parse_source(
        {source_id{1}, text}, {}, operation_id{705}, context).ok()) return false;
    const parser_source_fact_batch batches[] = {
        {source_id{1}, &context, context.enums}};
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    if (!run_parser_builder(builder, transaction, batches, diagnostics).ok() ||
        !transaction.commit().ok()) return false;
    const auto first = manager.strings().find("Growth0");
    const auto last = manager.strings().find("Growth3999");
    return first && last && manager.compiled_graph().find(first) &&
           manager.compiled_graph().find(last);
}

bool test_canonicalization_failure_stops_later_sources()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 2)) return false;
    source_context malformed;
    enum_declaration_source_fact bad;
    bad.canonical_name = {999, 7};
    bad.definition_state = enum_definition_state::opaque;
    malformed.enums.push_back(bad);

    source_context later;
    if (!parse_source(
        {source_id{2}, "enum Later : int;"}, {}, operation_id{706}, later).ok())
        return false;
    const parser_source_fact_batch batches[] = {
        {source_id{1}, &malformed, malformed.enums},
        {source_id{2}, &later, later.enums}};
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    const auto result = run_parser_builder(builder, transaction, batches, diagnostics);
    return !result.ok() && !transaction.commit().ok() &&
           !manager.strings().find("Later") &&
           manager.compiled_graph().entity_count() == 0 &&
           diagnostics.records().size() == 1;
}

bool test_parser_enum_p0_correctness()
{
    const auto rejected = [](std::string_view text, diagnostic_id expected)
    {
        source_context context;
        const auto result = parse_source(
            {source_id{1}, text}, {}, operation_id{707}, context);
        return !result.ok() && context.diagnostics.records().size() == 1 &&
               context.diagnostics.records()[0].id == expected;
    };
    if (!rejected("enum E;",
                  diagnostics::parser_invalid_enum_forward_declaration.id) ||
        !rejected("enum;", diagnostics::parser_anonymous_opaque_enum.id) ||
        !rejected("enum E { A, A };",
                  diagnostics::parser_duplicate_enumerator.id))
        return false;

    source_context maximum_signed;
    if (!parse_source({source_id{1},
            "enum E : long long { A = 9223372036854775807 };"}, {},
            operation_id{707}, maximum_signed).ok()) return false;
    const auto signed_values = maximum_signed.enumerators(maximum_signed.enums[0]);
    if (signed_values.size() != 1 ||
        signed_values[0].value.type != builtin_type::long_long_integer ||
        signed_values[0].value.bits != 0x7fffffffffffffffULL) return false;

    source_context maximum_unsigned;
    if (!parse_source({source_id{1},
            "enum E : unsigned long long { A = 18446744073709551615 };"}, {},
            operation_id{707}, maximum_unsigned).ok()) return false;
    const auto unsigned_values = maximum_unsigned.enumerators(maximum_unsigned.enums[0]);
    return unsigned_values.size() == 1 &&
           unsigned_values[0].value.type == builtin_type::unsigned_long_long_integer &&
           unsigned_values[0].value.bits == 0xffffffffffffffffULL &&
           rejected("enum E : long long { A = 9223372036854775807, B };",
                    diagnostics::parser_invalid_enumerator_expression.id);
}

bool test_parser_p1_environment_expressions_and_provenance()
{
    const source_constant_binding bindings[] = {
        {{}, "External", {builtin_type::long_long_integer, 7}},
        {{}, "X", {builtin_type::long_long_integer, 1}},
        {"N", "X", {builtin_type::long_long_integer, 2}}};
    source_environment_storage environment_storage;
    const source_type_binding types[] = {
        {{}, "B", "B"}, {"N", "B", "N::B"}};
    if (!environment_storage.initialize(bindings, types).ok()) return false;
    const source_environment environment{environment_storage};
    if (environment_storage.initialize({}, {}).ok()) return false;
    source_context context;
    if (!parse_source({source_id{1},
            "enum E : int { A = External + 5, B = A + 1 };"},
            environment, operation_id{708}, context).ok()) return false;
    if (context.enums.size() != 1 ||
        context.enums[0].declaration_range.length == 0 ||
        context.enums[0].name_range.length == 0) return false;
    const auto values = context.enumerators(context.enums[0]);
    if (!(values.size() == 2 && values[0].value.bits == 12 &&
           values[1].value.bits == 13 && values[0].name_range.length == 1 &&
           values[0].expression_range.length != 0 &&
           values[1].expression_range.length != 0)) return false;
    source_context nested;
    if (!parse_source({source_id{1},
            "namespace N { namespace M { enum E { A = X }; } }"},
            environment, operation_id{708}, nested).ok()) return false;
    const auto nested_values = nested.enumerators(nested.enums[0]);
    std::string_view resolved_type;
    return nested_values.size() == 1 && nested_values[0].value.bits == 2 &&
           environment.find_type_exact("N", "B", resolved_type).ok() &&
           resolved_type == "N::B";
}

bool test_parser_struct_declarations_and_empty_definitions()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    source_context context;
    constexpr std::string_view text =
        "struct B; struct B; struct B {}; struct Standalone {}; "
        "namespace N { struct C; }";
    if (!parse_source({source_id{1}, text}, {}, operation_id{715}, context).ok() ||
        context.aggregates.size() != 5) return false;
    const parser_source_fact_batch batch{
        source_id{1}, &context, context.enums, context.aggregates};
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    if (!publish_source_facts(transaction, batch, builder, operation_id{715},
                              diagnostics).ok() ||
        !transaction.commit().ok()) return false;
    const auto b_name = manager.strings().find("B");
    const auto standalone_name = manager.strings().find("Standalone");
    const auto nested_name = manager.strings().find("N::C");
    const auto* b = manager.compiled_graph().find(b_name);
    const auto* standalone = manager.compiled_graph().find(standalone_name);
    const auto* nested = manager.compiled_graph().find(nested_name);
    const auto* b_type = b ? manager.compiled_graph().find(b->type) : nullptr;
    const auto* standalone_type = standalone
        ? manager.compiled_graph().find(standalone->type) : nullptr;
    const auto* nested_type = nested
        ? manager.compiled_graph().find(nested->type) : nullptr;
    if (!b || !standalone || !nested || !b_type || !standalone_type || !nested_type ||
        b_type->aggregate.definition_state != aggregate_definition_state::defined ||
        standalone_type->aggregate.definition_state != aggregate_definition_state::defined ||
        nested_type->aggregate.definition_state != aggregate_definition_state::declared)
        return false;

    const auto rejected = [](std::string_view invalid)
    {
        source_context rejected_context;
        const auto result = parse_source(
            {source_id{1}, invalid}, {}, operation_id{715}, rejected_context);
        return !result.ok() && rejected_context.aggregates.empty() &&
               rejected_context.diagnostics.records().size() == 1;
    };
    if (!rejected("struct ;") || !rejected("struct Missing") ||
        !rejected("struct Open {") ||
        !rejected("struct NoSemicolon {}")) return false;

    // Canonical duplicate definitions fail, preserving the committed definition.
    source_context duplicate;
    if (!parse_source({source_id{1}, "struct B {}; struct B {};"}, {},
                      operation_id{715}, duplicate).ok()) return false;
    const parser_source_fact_batch duplicate_batch{
        source_id{1}, &duplicate, duplicate.enums, duplicate.aggregates};
    auto failed = manager.begin_build();
    const auto result = publish_source_facts(
        failed, duplicate_batch, builder, operation_id{715}, diagnostics);
    return !result.ok() && !failed.commit().ok() &&
           manager.compiled_graph().find(b_name) &&
           manager.compiled_graph().find(b_name)->id == b->id;
}

bool test_publication_semantic_failure_has_exactly_one_diagnostic()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 2)) return false;
    source_context first;
    source_context second;
    constexpr std::string_view text = "enum E : int { A = 1 };";
    if (!parse_source({source_id{1}, text}, {}, operation_id{709}, first).ok() ||
        !parse_source({source_id{2}, text}, {}, operation_id{709}, second).ok())
        return false;
    const parser_source_fact_batch batches[] = {
        {source_id{1}, &first, first.enums},
        {source_id{2}, &second, second.enums}};
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    status result;
    for (const auto& batch : batches)
    {
        result = publish_source_facts(
            transaction, batch, builder, operation_id{709}, diagnostics);
        if (!result.ok()) break;
    }
    return !result.ok() && !transaction.commit().ok() &&
           manager.state() == project_state::error &&
           diagnostics.records().size() == 1 &&
           diagnostics.records()[0].id == diagnostics::builder_semantic_failure.id &&
           diagnostics.records()[0].location.source == source_id{2} &&
           diagnostics.records()[0].location.length != 0;
}

bool test_frontend_generation_processes_source_once()
{
    const auto path = std::filesystem::absolute(
        "out/frontend_generation_once_A.hpp").lexically_normal();
    const auto included_path = std::filesystem::absolute(
        "out/frontend_generation_once_B.hpp").lexically_normal();
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "#include \"frontend_generation_once_B.hpp\"\n"
                  "enum OnceA : int { Value = 1 };";
        if (!output) return false;
    }
    {
        std::ofstream output(included_path, std::ios::binary | std::ios::trunc);
        output << "enum OnceB : int { Value = 2 };";
        if (!output) return false;
    }
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id source;
    if (!transaction.sources().resolve(
            path, project_item_role::source, source).ok()) return false;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    if (!transaction.sources().acquire(source, telemetry).ok()) return false;
    source_frontend_generation frontend{transaction};
    source_context context;
    project_builder builder;
    diagnostic_buffer graph_diagnostics;
    source_id work;
    if (!frontend.enqueue(source).ok() || !frontend.enqueue(source).ok() ||
        !frontend.take_discovery(work) || work != source ||
        !frontend.discover(work, operation_id{710}, graph_diagnostics).ok())
        return false;
    if (transaction.sources().includes(source).size() != 1) return false;
    const auto included = transaction.sources().includes(source)[0];
    if (!frontend.take_discovery(work) || work != included ||
        !transaction.sources().acquire(included, telemetry).ok() ||
        !frontend.discover(work, operation_id{710}, graph_diagnostics).ok() ||
        !frontend.finish_discovery(operation_id{710}, graph_diagnostics).ok())
        return false;
    if (!frontend.take_semantic_ready(work) || work != included ||
        !frontend.parse_and_publish(work, operation_id{710}, context, builder).ok() ||
        !frontend.take_semantic_ready(work) || work != source ||
        !frontend.parse_and_publish(work, operation_id{710}, context, builder).ok())
        return false;
    const auto source_counts = frontend.counts(source);
    const auto included_counts = frontend.counts(included);
    if (source_counts.discovery != 1 || source_counts.lex != 1 ||
        source_counts.parse != 1 || source_counts.publish != 1 ||
        included_counts.discovery != 1 || included_counts.lex != 1 ||
        included_counts.parse != 1 || included_counts.publish != 1 ||
        !transaction.commit().ok()) return false;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(included_path, ignored);
    return manager.compiled_graph().entity_count() == 2;
}

bool test_lexer_emits_sparse_directive_index()
{
    constexpr std::string_view text =
        "#include \"A.hpp\"\n"
        "enum E : int { A = 1 };\n"
        "  #include \"B.hpp\"\n";
    diagnostic_buffer diagnostics;
    std::vector<parser_token> tokens;
    std::vector<directive_span> directives;
    if (!lex_source({source_id{1}, text}, operation_id{711}, diagnostics,
                    tokens, &directives).ok() || directives.size() != 2)
        return false;
    for (const auto span : directives)
    {
        if (span.token_end - span.token_begin != 3 ||
            tokens[span.token_begin].punctuation != parser_punctuation::hash ||
            tokens[span.token_begin + 1].kind != parser_token_kind::identifier ||
            tokens[span.token_begin + 2].kind != parser_token_kind::string_literal)
            return false;
    }
    return tokens.back().kind == parser_token_kind::eof;
}

bool test_missing_include_fails_without_edge()
{
    const auto path = std::filesystem::absolute(
        "out/scheduler_missing_include_A.hpp").lexically_normal();
    { std::ofstream file(path); file << "#include \"NeverExists.hpp\"\n"; }
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id source;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    if (!transaction.sources().resolve(path, project_item_role::source, source).ok() ||
        !transaction.sources().acquire(source, telemetry).ok()) return false;
    source_frontend_generation scheduler{transaction};
    diagnostic_buffer diagnostics;
    source_id work;
    const bool started = scheduler.enqueue(source).ok() &&
        scheduler.take_discovery(work) && work == source;
    const auto result = started
        ? scheduler.discover(work, operation_id{715}, diagnostics)
        : status{status_code::invalid_state};
    const bool correct = result.code == status_code::configuration_failed &&
        transaction.sources().includes(source).empty() && scheduler.failed() &&
        !transaction.commit().ok() && manager.state() == project_state::error &&
        diagnostics.records().size() == 1 &&
        diagnostics.records()[0].id == diagnostics::source_include_not_found.id;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return correct;
}

bool test_unsupported_directive_fails_closed()
{
    const auto path = std::filesystem::absolute(
        "out/scheduler_unsupported_define.hpp").lexically_normal();
    { std::ofstream file(path); file << "#define X 10\n"; }
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id source;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    if (!transaction.sources().resolve(path, project_item_role::source, source).ok() ||
        !transaction.sources().acquire(source, telemetry).ok()) return false;
    source_frontend_generation scheduler{transaction};
    diagnostic_buffer diagnostics;
    source_id work;
    const bool started = scheduler.enqueue(source).ok() &&
        scheduler.take_discovery(work) && work == source;
    const auto result = started
        ? scheduler.discover(work, operation_id{716}, diagnostics)
        : status{status_code::invalid_state};
    const bool correct = result.code == status_code::configuration_failed &&
        scheduler.failed() && !transaction.commit().ok() &&
        manager.state() == project_state::error &&
        diagnostics.records().size() == 1 &&
        diagnostics.records()[0].id == diagnostics::source_unsupported_directive.id;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return correct;
}

bool test_include_inside_namespace_fails_closed()
{
    const auto directory = std::filesystem::absolute("out").lexically_normal();
    const auto path = directory / "scheduler_namespace_include_A.hpp";
    const auto dependency = directory / "scheduler_namespace_include_B.hpp";
    { std::ofstream file(path); file << "namespace N {\n#include \"scheduler_namespace_include_B.hpp\"\n}\n"; }
    { std::ofstream file(dependency); file << "struct B;\n"; }
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id source;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    if (!transaction.sources().resolve(path, project_item_role::source, source).ok() ||
        !transaction.sources().acquire(source, telemetry).ok()) return false;
    source_frontend_generation scheduler{transaction};
    diagnostic_buffer diagnostics;
    source_id work;
    const bool started = scheduler.enqueue(source).ok() &&
        scheduler.take_discovery(work) && work == source;
    const auto result = started
        ? scheduler.discover(work, operation_id{717}, diagnostics)
        : status{status_code::invalid_state};
    const bool correct = result.code == status_code::configuration_failed &&
        scheduler.failed() && diagnostics.records().size() == 1 &&
        diagnostics.records()[0].id ==
            diagnostics::source_include_inside_namespace.id;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(dependency, ignored);
    return correct;
}

bool test_complete_rebuild_discovers_transitive_sources()
{
    const auto directory = std::filesystem::absolute("out").lexically_normal();
    const auto a = directory / "rebuild_A.hpp";
    const auto b = directory / "rebuild_B.hpp";
    const auto c = directory / "rebuild_C.hpp";
    const auto unrelated = directory / "rebuild_unrelated.hpp";
    { std::ofstream file(a); file << "#include \"rebuild_B.hpp\"\nstruct A;\n"; }
    { std::ofstream file(b); file << "#include \"rebuild_C.hpp\"\nstruct B;\n"; }
    { std::ofstream file(c); file << "struct C;\n"; }
    { std::ofstream file(unrelated); file << "struct Unrelated;\n"; }
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    if (manager.state() != project_state::building) return false;
    source_id root;
    if (!transaction.sources().resolve(a, project_item_role::source, root).ok())
        return false;
    language_configuration language;
    language.preprocessor.include = true;
    source_frontend_generation scheduler{transaction, language};
    source_acquisition_telemetry telemetry{metrics_mode::off};
    diagnostic_buffer diagnostics;
    project_builder builder;
    const auto result = scheduler.rebuild(operation_id{718}, diagnostics,
                                          telemetry, builder);
    const bool correct = result.ok() && !result.checkpoint.has_value() && diagnostics.empty() &&
        manager.compiled_graph().entity_count() == 3 &&
        manager.state() == project_state::valid;
    const graph* runnable = nullptr;
    const bool runtime_allowed = manager.runnable_graph(runnable).ok() &&
        runnable == &manager.compiled_graph();
    std::error_code ignored;
    std::filesystem::remove(a, ignored);
    std::filesystem::remove(b, ignored);
    std::filesystem::remove(c, ignored);
    std::filesystem::remove(unrelated, ignored);
    return correct && runtime_allowed;
}

bool test_complete_rebuild_missing_root_fails()
{
    const auto path = std::filesystem::absolute(
        "out/rebuild_missing_root.hpp").lexically_normal();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id root;
    if (!transaction.sources().resolve(path, project_item_role::source, root).ok())
        return false;
    source_frontend_generation scheduler{transaction};
    source_acquisition_telemetry telemetry{metrics_mode::off};
    diagnostic_buffer diagnostics;
    project_builder builder;
    const auto result = scheduler.rebuild(operation_id{719}, diagnostics,
                                          telemetry, builder);
    return result.semantic.code == status_code::configuration_failed &&
        scheduler.failed() && manager.state() == project_state::error &&
        manager.compiled_graph().entity_count() == 0 &&
        ([&]{ const graph* runnable = nullptr;
              return !manager.runnable_graph(runnable).ok() && !runnable; })() &&
        diagnostics.records().size() == 1 &&
        diagnostics.records()[0].id == diagnostics::source_acquisition_failed.id;
}

bool test_complete_rebuild_parser_failure_sets_error()
{
    const auto path = std::filesystem::absolute(
        "out/rebuild_parser_failure.hpp").lexically_normal();
    { std::ofstream file(path); file << "enum Broken { A = 1 }\n"; }
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id root;
    if (!transaction.sources().resolve(path, project_item_role::source, root).ok())
        return false;
    source_frontend_generation scheduler{transaction};
    source_acquisition_telemetry telemetry{metrics_mode::off};
    diagnostic_buffer diagnostics;
    project_builder builder;
    const auto result = scheduler.rebuild(operation_id{723}, diagnostics,
                                          telemetry, builder);
    const graph* runnable = nullptr;
    const bool correct = !result.ok() && manager.state() == project_state::error &&
        !manager.runnable_graph(runnable).ok() && runnable == nullptr &&
        !diagnostics.empty();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return correct;
}

bool test_checkpoint_failure_preserves_valid_project()
{
    const auto source = std::filesystem::absolute(
        "out/checkpoint_lifecycle.hpp").lexically_normal();
    const auto checkpoint = std::filesystem::absolute(
        "out/checkpoint_failure_target").lexically_normal();
    { std::ofstream file(source); file << "struct DurableInMemory;\n"; }
    std::error_code ignored;
    std::filesystem::remove_all(checkpoint, ignored);
    std::filesystem::create_directory(checkpoint, ignored);

    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id root;
    if (!transaction.sources().resolve(source, project_item_role::source, root).ok())
        return false;
    source_frontend_generation scheduler{transaction};
    source_acquisition_telemetry telemetry{metrics_mode::off};
    diagnostic_buffer diagnostics;
    project_builder builder;
    const auto result = scheduler.rebuild(operation_id{724}, diagnostics,
                                          telemetry, builder, checkpoint);
    const graph* runnable = nullptr;
    const bool correct = result.semantic.ok() && result.checkpoint.has_value() &&
        !result.checkpoint->ok() && manager.state() == project_state::valid &&
        manager.runnable_graph(runnable).ok() && runnable != nullptr &&
        manager.compiled_graph().entity_count() == 1 &&
        diagnostics.records().size() == 1 &&
        diagnostics.records()[0].id == diagnostics::source_checkpoint_save_failed.id &&
        std::filesystem::is_directory(checkpoint);
    std::filesystem::remove(source, ignored);
    std::filesystem::remove_all(checkpoint, ignored);
    return correct;
}

bool test_checkpoint_success_preserves_valid_project()
{
    const auto source = std::filesystem::absolute(
        "out/checkpoint_success.hpp").lexically_normal();
    const auto checkpoint = std::filesystem::absolute(
        "out/checkpoint_success.bin").lexically_normal();
    { std::ofstream file(source); file << "struct Checkpointed;\n"; }
    std::error_code ignored;
    std::filesystem::remove(checkpoint, ignored);
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id root;
    if (!transaction.sources().resolve(source, project_item_role::source, root).ok())
        return false;
    source_frontend_generation scheduler{transaction};
    source_acquisition_telemetry telemetry{metrics_mode::off};
    diagnostic_buffer diagnostics;
    project_builder builder;
    const auto result = scheduler.rebuild(operation_id{725}, diagnostics,
                                          telemetry, builder, checkpoint);
    const graph* runnable = nullptr;
    const bool correct = result.semantic.ok() && result.checkpoint.has_value() &&
        result.checkpoint->ok() && manager.state() == project_state::valid &&
        manager.runnable_graph(runnable).ok() && runnable != nullptr &&
        diagnostics.empty() && std::filesystem::is_regular_file(checkpoint);
    std::filesystem::remove(source, ignored);
    std::filesystem::remove(checkpoint, ignored);
    return correct;
}

bool test_failed_rebuild_has_no_runtime_fallback_and_recovers()
{
    const auto root_path = std::filesystem::absolute(
        "out/lifecycle_rebuild.hpp").lexically_normal();
    const auto write = [&](std::string_view text)
    {
        std::ofstream file(root_path, std::ios::binary | std::ios::trunc);
        file << text;
        return static_cast<bool>(file);
    };
    graph_manager manager;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    project_builder builder;
    if (!manager.initialize().ok() || !write("struct Stable;\n")) return false;

    {
        auto transaction = manager.begin_build();
        source_id root;
        if (!transaction.sources().resolve(root_path, project_item_role::source,
                                           root).ok()) return false;
        source_frontend_generation scheduler{transaction};
        diagnostic_buffer diagnostics;
        if (!scheduler.rebuild(operation_id{720}, diagnostics, telemetry,
                               builder).ok()) return false;
    }
    const auto old_count = manager.compiled_graph().entity_count();
    const graph* runnable = nullptr;
    if (old_count != 1 || manager.state() != project_state::valid ||
        !manager.runnable_graph(runnable).ok()) return false;

    if (!write("#include \"lifecycle_missing.hpp\"\n")) return false;
    diagnostic_buffer failure_diagnostics;
    {
        auto transaction = manager.begin_build();
        source_id root;
        if (!transaction.sources().resolve(root_path, project_item_role::source,
                                           root).ok()) return false;
        source_frontend_generation scheduler{transaction};
        if (scheduler.rebuild(operation_id{721}, failure_diagnostics, telemetry,
                              builder).ok()) return false;
    }
    runnable = nullptr;
    if (manager.state() != project_state::error ||
        manager.runnable_graph(runnable).ok() || runnable != nullptr ||
        manager.compiled_graph().entity_count() != old_count ||
        failure_diagnostics.empty()) return false;

    if (!write("struct Stable;\n")) return false;
    {
        auto transaction = manager.begin_build();
        source_id root;
        if (!transaction.sources().resolve(root_path, project_item_role::source,
                                           root).ok()) return false;
        source_frontend_generation scheduler{transaction};
        diagnostic_buffer diagnostics;
        if (!scheduler.rebuild(operation_id{722}, diagnostics, telemetry,
                               builder).ok()) return false;
    }
    const bool recovered = manager.state() == project_state::valid &&
        manager.runnable_graph(runnable).ok() && runnable != nullptr;
    std::error_code ignored;
    std::filesystem::remove(root_path, ignored);
    return recovered;
}

bool test_scheduler_shared_dependency_and_fanout()
{
    const auto directory = std::filesystem::absolute("out").lexically_normal();
    const auto a_path = directory / "scheduler_A.hpp";
    const auto d_path = directory / "scheduler_D.hpp";
    const auto common_path = directory / "scheduler_common.hpp";
    { std::ofstream f(a_path); f << "#include \"scheduler_common.hpp\"\n"
                                   "enum SchedulerA : int { A = 1 };"; if (!f) return false; }
    { std::ofstream f(d_path); f << "#include \"scheduler_common.hpp\"\n"
                                   "enum SchedulerD : int { D = 2 };"; if (!f) return false; }
    { std::ofstream f(common_path); f << "struct SharedB; enum SchedulerCommon : int { C = 3 };";
      if (!f) return false; }

    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id a, d;
    if (!transaction.sources().resolve(a_path, project_item_role::source, a).ok() ||
        !transaction.sources().resolve(d_path, project_item_role::source, d).ok())
        return false;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    if (!transaction.sources().acquire(a, telemetry).ok() ||
        !transaction.sources().acquire(d, telemetry).ok()) return false;
    source_frontend_generation scheduler{transaction};
    diagnostic_buffer diagnostics;
    source_id work;
    if (!scheduler.enqueue(a).ok() || !scheduler.enqueue(d).ok() ||
        !scheduler.take_discovery(work) || work != a ||
        !scheduler.discover(work, operation_id{712}, diagnostics).ok() ||
        !scheduler.take_discovery(work) || work != d ||
        !scheduler.discover(work, operation_id{712}, diagnostics).ok()) return false;
    const auto a_dependencies = transaction.sources().includes(a);
    const auto d_dependencies = transaction.sources().includes(d);
    if (a_dependencies.size() != 1 || d_dependencies.size() != 1 ||
        a_dependencies[0] != d_dependencies[0] ||
        scheduler.take_semantic_ready(work)) return false;
    const auto common = a_dependencies[0];
    if (!scheduler.take_discovery(work) || work != common ||
        !transaction.sources().acquire(common, telemetry).ok() ||
        !scheduler.discover(work, operation_id{712}, diagnostics).ok() ||
        scheduler.take_discovery(work) ||
        !scheduler.finish_discovery(operation_id{712}, diagnostics).ok()) return false;

    source_context first_context;
    project_builder builder;
    if (!scheduler.take_semantic_ready(work) || work != common ||
        !scheduler.parse_and_publish(work, operation_id{712},
                                     first_context, builder).ok()) return false;
    const auto* common_interface = scheduler.interface(common);
    if (!common_interface) return false;
    first_context.reset();
    source_name_ref disturbed;
    const std::string overwrite(64 * 1024, 'z');
    if (!first_context.store_name(overwrite, disturbed).ok()) return false;
    const source_environment common_view{*common_interface};
    std::string_view canonical;
    integral_constant constant;
    if (!common_view.find_type_exact({}, "SchedulerCommon", canonical).ok() ||
        canonical != "SchedulerCommon" ||
        !common_view.find_type_exact({}, "SharedB", canonical).ok() ||
        canonical != "SharedB" ||
        !common_view.find_constant_exact({}, "C", constant).ok() ||
        constant.bits != 3 ||
        common_view.find_constant_exact("Parent", "C", constant).ok()) return false;
    std::atomic<bool> reads_ok{true};
    std::thread readers[4];
    for (auto& reader : readers)
        reader = std::thread([&]
        {
            for (int iteration = 0; iteration != 1000; ++iteration)
            {
                integral_constant value;
                if (!common_view.find_constant_exact({}, "C", value).ok() ||
                    value.bits != 3) reads_ok.store(false, std::memory_order_relaxed);
            }
        });
    for (auto& reader : readers) reader.join();
    if (!reads_ok.load(std::memory_order_relaxed)) return false;
    source_id first_ready, second_ready;
    if (!scheduler.take_semantic_ready(first_ready) ||
        !scheduler.take_semantic_ready(second_ready) || first_ready == second_ready ||
        !((first_ready == a && second_ready == d) ||
          (first_ready == d && second_ready == a))) return false;
    if (!scheduler.parse_and_publish(first_ready, operation_id{712},
                                     first_context, builder).ok()) return false;
    first_context.reset();
    if (!scheduler.parse_and_publish(second_ready, operation_id{712},
                                     first_context, builder).ok()) return false;
    first_context.reset();
    const auto* a_interface = scheduler.interface(a);
    const auto* d_interface = scheduler.interface(d);
    if (!a_interface || !d_interface) return false;
    const source_environment a_view{*a_interface};
    const source_environment d_view{*d_interface};
    if (!a_view.find_constant_exact({}, "A", constant).ok() || constant.bits != 1 ||
        !d_view.find_constant_exact({}, "D", constant).ok() || constant.bits != 2)
        return false;
    const auto common_counts = scheduler.counts(common);
    if (common_counts.discovery != 1 || common_counts.lex != 1 ||
        common_counts.parse != 1 || common_counts.publish != 1 ||
        scheduler.interface(common) != common_interface ||
        !transaction.commit().ok()) return false;
    std::error_code ignored;
    std::filesystem::remove(a_path, ignored);
    std::filesystem::remove(d_path, ignored);
    std::filesystem::remove(common_path, ignored);
    return manager.compiled_graph().entity_count() == 4;
}

bool test_scheduler_cycle_fails_without_waiting()
{
    const auto directory = std::filesystem::absolute("out").lexically_normal();
    const auto a_path = directory / "scheduler_cycle_A.hpp";
    const auto b_path = directory / "scheduler_cycle_B.hpp";
    const auto c_path = directory / "scheduler_cycle_C.hpp";
    { std::ofstream f(a_path); f << "#include \"scheduler_cycle_B.hpp\"\n"; }
    { std::ofstream f(b_path); f << "#include \"scheduler_cycle_C.hpp\"\n"; }
    { std::ofstream f(c_path); f << "#include \"scheduler_cycle_A.hpp\"\n"; }
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id a;
    if (!transaction.sources().resolve(a_path, project_item_role::source, a).ok())
        return false;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    if (!transaction.sources().acquire(a, telemetry).ok()) return false;
    source_frontend_generation scheduler{transaction};
    diagnostic_buffer diagnostics;
    if (!scheduler.enqueue(a).ok()) return false;
    source_id work;
    while (scheduler.take_discovery(work))
    {
        source_physical_state physical;
        if (!transaction.sources().get_physical_state(work, physical).ok() &&
            !transaction.sources().acquire(work, telemetry).ok()) return false;
        if (!scheduler.discover(work, operation_id{713}, diagnostics).ok())
            return false;
    }
    const auto result = scheduler.finish_discovery(operation_id{713}, diagnostics);
    const bool correct = result.code == status_code::configuration_failed &&
        scheduler.failed() && !transaction.commit().ok() &&
        manager.state() == project_state::error &&
        diagnostics.records().size() == 1 &&
        diagnostics.records()[0].id == diagnostics::source_include_cycle.id;
    std::error_code ignored;
    std::filesystem::remove(a_path, ignored);
    std::filesystem::remove(b_path, ignored);
    std::filesystem::remove(c_path, ignored);
    return correct;
}

bool test_scheduler_dependency_failure_blocks_dependent()
{
    const auto directory = std::filesystem::absolute("out").lexically_normal();
    const auto a_path = directory / "scheduler_failure_A.hpp";
    const auto b_path = directory / "scheduler_failure_B.hpp";
    { std::ofstream f(a_path); f << "#include \"scheduler_failure_B.hpp\"\n"
                                   "enum ShouldNotParse : int { A = 1 };"; }
    { std::ofstream f(b_path); f << "@"; }
    graph_manager manager;
    if (!manager.initialize().ok()) return false;
    auto transaction = manager.begin_build();
    source_id a;
    if (!transaction.sources().resolve(a_path, project_item_role::source, a).ok())
        return false;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    if (!transaction.sources().acquire(a, telemetry).ok()) return false;
    source_frontend_generation scheduler{transaction};
    diagnostic_buffer diagnostics;
    source_id work;
    if (!scheduler.enqueue(a).ok() || !scheduler.take_discovery(work) ||
        !scheduler.discover(work, operation_id{714}, diagnostics).ok()) return false;
    const auto dependency = transaction.sources().includes(a)[0];
    if (!scheduler.take_discovery(work) || work != dependency ||
        !transaction.sources().acquire(dependency, telemetry).ok()) return false;
    const auto result = scheduler.discover(work, operation_id{714}, diagnostics);
    const auto a_counts = scheduler.counts(a);
    const bool correct = !result.ok() && scheduler.failed() &&
        a_counts.parse == 0 && a_counts.publish == 0 &&
        scheduler.interface(a) == nullptr &&
        scheduler.interface(dependency) == nullptr &&
        !scheduler.take_semantic_ready(work) && !transaction.commit().ok();
    std::error_code ignored;
    std::filesystem::remove(a_path, ignored);
    std::filesystem::remove(b_path, ignored);
    return correct;
}

bool test_aggregate_member_core()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    constexpr std::string_view text = "struct S { int& IN; int OUT; };";
    source_context context;
    if (!parse_source({source_id{1}, text}, {}, operation_id{715}, context).ok())
        return false;
    const parser_source_fact_batch batch{
        source_id{1}, &context, context.enums, context.aggregates};
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    if (!publish_source_facts(transaction, batch, builder, operation_id{715}, diagnostics).ok() ||
        !transaction.commit().ok()) return false;
    const auto s_name = manager.strings().find("S");
    const auto in_name = manager.strings().find("IN");
    const auto out_name = manager.strings().find("OUT");
    const auto* entity = manager.compiled_graph().find(s_name);
    if (!entity || manager.compiled_graph().member_count(entity->type) != 2)
        return false;
    const auto in_index = manager.compiled_graph().find_member(entity->type, in_name);
    const auto out_index = manager.compiled_graph().find_member(entity->type, out_name);
    const auto* in = manager.compiled_graph().member(entity->type, in_index);
    const auto* out = manager.compiled_graph().member(entity->type, out_index);
    const auto* reference = in ? manager.compiled_graph().derived(in->type) : nullptr;
    builtin_type decoded{};
    return in_index.value() == 1 && out_index.value() == 2 && in && out &&
        reference && reference->kind == derived_type_kind::lvalue_reference &&
        manager.compiled_graph().builtin(reference->child,decoded) &&
        decoded == builtin_type::integer &&
        manager.compiled_graph().builtin(out->type,decoded) && decoded==builtin_type::integer;
}

bool test_duplicate_aggregate_member_fails_atomically()
{
    graph_manager manager;
    if (!manager.initialize().ok() || !initialize_sources(manager, 1)) return false;
    source_context context;
    constexpr std::string_view text = "struct S { int A; int A; };";
    if (!parse_source({source_id{1}, text}, {}, operation_id{716}, context).ok())
        return false;
    const parser_source_fact_batch batch{
        source_id{1}, &context, context.enums, context.aggregates};
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    const auto result = publish_source_facts(
        transaction, batch, builder, operation_id{716}, diagnostics);
    return !result.ok() && !transaction.commit().ok() &&
        manager.compiled_graph().entity_count() == 0 &&
        manager.compiled_graph().user_type_count() == 0;
}

} // namespace

int main()
{
    const std::pair<const char*, bool(*)()> tests[] = {
        {"named_definition", test_named_definition},
        {"opaque_definition_and_replacement", test_opaque_definition_and_replacement},
        {"repeated_opaque_and_incompatible_atomicity", test_repeated_opaque_and_incompatible_atomicity},
        {"duplicate_definition_atomicity", test_duplicate_definition_atomicity},
        {"remove_final_and_no_id_reuse", test_remove_final_and_no_id_reuse},
        {"move_definition_and_omitted_source", test_move_definition_and_omitted_source},
        {"anonymous_lifetime", test_anonymous_lifetime},
        {"structural_validation_aborts_transaction", test_structural_validation_aborts_transaction},
        {"duplicate_source_batches", test_duplicate_source_batch_cannot_publish_merged_state},
        {"replace_source_open_boundary", test_replace_source_reports_duplicate_at_open_boundary},
        {"permutation_of_existing_identity", test_permutation_of_existing_identity},
        {"supplied_order", test_supplied_source_and_declaration_order_is_preserved},
        {"worker_completion_order", test_worker_completion_order_is_resolved_before_builder},
        {"parser_facts_detached", test_parser_facts_are_canonicalized_and_detached_before_publication},
        {"parser_worker_completion", test_parser_worker_completion_does_not_change_canonical_ids},
        {"parser_fail_fast", test_parser_fail_fast_and_preserves_committed_graph},
        {"source_name_ref_growth", test_source_name_refs_survive_context_growth},
        {"canonicalization_fail_fast", test_canonicalization_failure_stops_later_sources},
        {"parser_enum_p0", test_parser_enum_p0_correctness},
        {"parser_p1_environment", test_parser_p1_environment_expressions_and_provenance},
        {"parser_structs", test_parser_struct_declarations_and_empty_definitions},
        {"publication_diagnostic", test_publication_semantic_failure_has_exactly_one_diagnostic},
        {"frontend_generation_once", test_frontend_generation_processes_source_once},
        {"lexer_directive_index", test_lexer_emits_sparse_directive_index},
        {"missing_include", test_missing_include_fails_without_edge},
        {"unsupported_directive", test_unsupported_directive_fails_closed},
        {"namespace_include", test_include_inside_namespace_fails_closed},
        {"complete_rebuild", test_complete_rebuild_discovers_transitive_sources},
        {"missing_root", test_complete_rebuild_missing_root_fails},
        {"parser_lifecycle", test_complete_rebuild_parser_failure_sets_error},
        {"checkpoint_failure_lifecycle", test_checkpoint_failure_preserves_valid_project},
        {"checkpoint_success_lifecycle", test_checkpoint_success_preserves_valid_project},
        {"lifecycle_recovery", test_failed_rebuild_has_no_runtime_fallback_and_recovers},
        {"scheduler_shared_dependency", test_scheduler_shared_dependency_and_fanout},
        {"scheduler_cycle", test_scheduler_cycle_fails_without_waiting},
        {"scheduler_dependency_failure", test_scheduler_dependency_failure_blocks_dependent},
        {"aggregate_member_core", test_aggregate_member_core},
        {"aggregate_member_duplicate", test_duplicate_aggregate_member_fails_atomically}};
    for (const auto& [name, test] : tests)
        if (!test()) { std::cerr << name << " failed\n"; return 1; }
    return 0;
}
