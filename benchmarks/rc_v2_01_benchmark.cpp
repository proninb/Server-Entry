#include "../server_entry/project/graph/graph_build_transaction_test_access.hpp"
#include "../server_entry/tests/rc_v2_01a_gates.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cw::server;
using access = graph_build_transaction_test_access;
using clock_type = std::chrono::steady_clock;

struct row {
    std::size_t types = 0;
    const char* scenario = nullptr;
    double total_ms = 0;
    double setup_ms = 0;
    double prepare_ms = 0;
    double publish_ms = 0;
    graph_storage_prepare_telemetry graph{};
    bool contribution_reallocated = false;
    std::size_t contribution_relocation_bytes = 0;
};

[[noreturn]] void fail(const char* message) {
    std::cerr << "RC-V2-01A FAIL: " << message << '\n';
    std::exit(2);
}

void require(bool condition, const char* message) {
    if (!condition) {
        fail(message);
    }
}

double milliseconds(clock_type::duration value) {
    return std::chrono::duration<double, std::milli>(value).count();
}

std::filesystem::path source_path(std::size_t index) {
    return std::filesystem::path{
        L"C:\\rc-v2-01\\source_" +
        std::to_wstring(index) +
        L".cpp"
    };
}

std::string type_name(std::size_t index) {
    return "T" + std::to_string(index);
}

struct prepared_type {
    source_id source{};
    string_id name{};
};

prepared_type prepare_source(
    graph_build_transaction& transaction,
    std::size_t index,
    std::string_view spelling) {

    prepared_type result;

    require(
        transaction.sources().resolve(
            source_path(index),
            project_item_role::source,
            result.source).ok(),
        "Source resolution failed");

    require(
        transaction.strings().intern(
            spelling,
            result.name).ok(),
        "String interning failed");

    return result;
}

void replace_with_opaque_enum(
    graph_build_transaction& transaction,
    source_id source,
    string_id name) {

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            source,
            replacement).ok(),
        "Source replacement failed");

    const enum_build_data data{
        enum_definition_state::opaque,
        false,
        builtin_type::integer,
        {}
    };

    stable_id entity;
    type_handle type;

    require(
        replacement.add_named_enum(
            name,
            data,
            entity,
            type).ok(),
        "Enum materialization failed");
}

void replace_empty(
    graph_build_transaction& transaction,
    source_id source) {

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            source,
            replacement).ok(),
        "Empty Source replacement failed");
}

row finish_row(
    graph_manager& manager,
    graph_build_transaction& transaction,
    std::size_t type_count,
    const char* scenario,
    clock_type::time_point started,
    clock_type::time_point setup_done,
    source_contribution_storage_snapshot contribution_before) {

    const auto prepared =
        access::prepare(transaction);

    const auto prepare_done =
        clock_type::now();

    require(prepared.ok(), "Transaction prepare failed");

    const auto graph_telemetry =
        access::graph_telemetry(transaction);

    const auto contribution_after_prepare =
        access::contribution_storage(manager);

    access::publish(transaction);

    const auto publish_done =
        clock_type::now();

    require(
        access::state(transaction) ==
            graph_build_transaction_state::committed,
        "Transaction did not publish");

    row result;
    result.types = type_count;
    result.scenario = scenario;
    result.setup_ms = milliseconds(setup_done - started);
    result.prepare_ms = milliseconds(prepare_done - setup_done);
    result.publish_ms = milliseconds(publish_done - prepare_done);
    result.total_ms = milliseconds(publish_done - started);
    result.graph = graph_telemetry;
    result.contribution_reallocated =
        rc_v2_01a::contribution_reallocated(
            contribution_before,
            contribution_after_prepare);
    result.contribution_relocation_bytes =
        rc_v2_01a::contribution_relocation_bytes(
            contribution_before,
            contribution_after_prepare);

    return result;
}

row g0_initial(
    graph_manager& manager,
    std::size_t count,
    source_id& target_source,
    string_id& target_name) {

    const auto contribution_before =
        access::contribution_storage(manager);

    const auto started = clock_type::now();

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    for (std::size_t index = 0;
         index < count;
         ++index) {
        const auto name = type_name(index);
        const auto prepared =
            prepare_source(
                transaction,
                index,
                name);

        replace_with_opaque_enum(
            transaction,
            prepared.source,
            prepared.name);

        if (index == count / 2) {
            target_source = prepared.source;
            target_name = prepared.name;
        }
    }

    const auto setup_done = clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        "g0_initial",
        started,
        setup_done,
        contribution_before);
}

row g1_modify_one(
    graph_manager& manager,
    std::size_t count,
    source_id target_source,
    string_id target_name) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto started = clock_type::now();

    auto transaction =
        manager.begin_build(graph_build_mode::incremental);

    replace_with_opaque_enum(
        transaction,
        target_source,
        target_name);

    const auto setup_done = clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        "g1_modify_one",
        started,
        setup_done,
        contribution_before);
}

row g2_remove_one(
    graph_manager& manager,
    std::size_t count,
    source_id target_source) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto started = clock_type::now();

    auto transaction =
        manager.begin_build(graph_build_mode::incremental);

    replace_empty(transaction, target_source);

    const auto setup_done = clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        "g2_remove_one",
        started,
        setup_done,
        contribution_before);
}

row g3_add_one(
    graph_manager& manager,
    std::size_t count,
    source_id target_source,
    string_id& replacement_name) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto started = clock_type::now();

    auto transaction =
        manager.begin_build(graph_build_mode::incremental);

    require(
        transaction.strings().intern(
            "T_replacement_" + std::to_string(count),
            replacement_name).ok(),
        "Replacement name interning failed");

    replace_with_opaque_enum(
        transaction,
        target_source,
        replacement_name);

    const auto setup_done = clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        "g3_add_one",
        started,
        setup_done,
        contribution_before);
}

row g0_rebuild_after_churn(
    graph_manager& manager,
    std::size_t count,
    source_id target_source,
    string_id replacement_name) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto started = clock_type::now();

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    for (std::size_t index = 0;
         index < count;
         ++index) {
        source_id source;

        require(
            transaction.sources().resolve(
                source_path(index),
                project_item_role::source,
                source).ok(),
            "Rebuild Source resolution failed");

        string_id name;

        if (source == target_source) {
            name = replacement_name;
        }
        else {
            require(
                transaction.strings().intern(
                    type_name(index),
                    name).ok(),
                "Rebuild name interning failed");
        }

        replace_with_opaque_enum(
            transaction,
            source,
            name);
    }

    const auto setup_done = clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        "g0_rebuild_after_churn",
        started,
        setup_done,
        contribution_before);
}

void require_incremental_gates(
    const row& value) {

    if (!rc_v2_01a::sparse_independent_gate(
            value.graph)) {
        const auto& g = value.graph;

        std::cerr
            << "SPARSE_GATE_DIAG scenario=" << value.scenario
            << " types=" << value.types
            << " changed_sources=" << g.changed_sources
            << " changed_entities=" << g.changed_entities
            << " changed_types=" << g.changed_types
            << " visited_types=" << g.validation_visited_types
            << " visited_type_refs=" << g.validation_visited_type_refs
            << " dependency_edges=" << g.validation_dependency_edges
            << " graph_reallocated="
            << (rc_v2_01a::graph_reallocated(g) ? 1 : 0)
            << '\n';

        const auto print_growth =
            [](const char* name,
               const graph_vector_growth_telemetry& item) {
                std::cerr
                    << "  " << name
                    << " size=" << item.size_before
                    << "->" << item.size_after
                    << " capacity=" << item.capacity_before
                    << "->" << item.capacity_after
                    << " reallocated=" << (item.reallocated ? 1 : 0)
                    << " relocation_bytes="
                    << item.relocation_payload_bytes
                    << '\n';
            };

        print_growth("identity", g.identity);
        print_growth("entities", g.entities);
        print_growth("types", g.types);
        print_growth("member_records", g.member_records);
        print_growth("enum_value_records", g.enum_value_records);
        print_growth("canonical_types", g.canonical_types);
        print_growth("named_type_refs", g.named_type_refs);
    }

    require(
        rc_v2_01a::sparse_independent_gate(
            value.graph),
        "Sparse independent validation gate failed");

    if (value.contribution_reallocated) {
        std::cerr
            << "SOURCE_CONTRIBUTION_DIAG scenario="
            << value.scenario
            << " relocation_bytes="
            << value.contribution_relocation_bytes
            << '\n';
    }

    require(
        !value.contribution_reallocated,
        "SourceContribution storage relocated for K=1");
}

void require_g0_headroom(
    const graph_manager& manager,
    std::size_t count) {

    if (count < 512) {
        return;
    }

    const auto storage =
        access::graph_storage(manager);

    require(
        storage.identity_capacity >
            storage.identity_size,
        "G0 identity headroom missing");

    require(
        storage.entities_capacity >
            storage.entities_size,
        "G0 Entity headroom missing");

    require(
        storage.types_capacity >
            storage.types_size,
        "G0 type headroom missing");

    const auto contributions =
        access::contribution_storage(manager);

    require(
        contributions.states_capacity >
            contributions.states_size,
        "G0 SourceContribution headroom missing");

    require(
        contributions.entity_states_capacity >
            contributions.entity_states_size,
        "G0 SourceContribution Entity headroom missing");
}

void fail_closed_gate() {
    graph_manager manager;
    require(manager.initialize().ok(), "Fail-closed manager init failed");

    source_id source;
    string_id name;

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        const auto prepared =
            prepare_source(transaction, 0, "Base");

        source = prepared.source;
        name = prepared.name;

        replace_with_opaque_enum(
            transaction,
            source,
            name);

        require(
            transaction.commit().ok(),
            "Fail-closed baseline commit failed");
    }

    const auto storage_before =
        access::graph_storage(manager);
    const auto generation_before =
        access::graph_generation(manager);
    const auto identity_before =
        manager.compiled_graph().find_id(name);

    auto transaction =
        manager.begin_build(graph_build_mode::incremental);

    string_id candidate_name;

    require(
        transaction.strings().intern(
            "Candidate",
            candidate_name).ok(),
        "Fail-closed candidate name failed");

    replace_with_opaque_enum(
        transaction,
        source,
        candidate_name);

    access::fail_after_graph_prepare(transaction);

    require(
        !transaction.commit().ok(),
        "Post-Graph-prepare failure injection did not fail");

    const auto storage_after =
        access::graph_storage(manager);

    require(
        access::graph_generation(manager) ==
            generation_before,
        "Failed prepare changed Graph generation");

    require(
        storage_after.identity_size ==
            storage_before.identity_size &&
        storage_after.entities_size ==
            storage_before.entities_size &&
        storage_after.types_size ==
            storage_before.types_size,
        "Failed prepare changed committed logical Graph size");

    require(
        manager.compiled_graph().find_id(name) ==
            identity_before &&
        !manager.compiled_graph().find_id(candidate_name),
        "Failed prepare changed committed Graph content");
}

void define_empty_aggregate(
    graph_build_transaction& transaction,
    std::size_t source_index,
    std::string_view name,
    std::string_view dependency = {}) {

    const auto prepared =
        prepare_source(
            transaction,
            source_index,
            name);

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            prepared.source,
            replacement).ok(),
        "Dependency Source replacement failed");

    stable_id entity;
    type_handle type;

    require(
        replacement.add_named_type(
            prepared.name,
            aggregate_definition_state::defined,
            entity,
            type).ok(),
        "Dependency aggregate creation failed");

    if (dependency.empty()) {
        require(
            replacement.define_members(
                type,
                {},
                {}).ok(),
            "Empty aggregate definition failed");
        return;
    }

    string_id dependency_name;

    require(
        transaction.strings().intern(
            dependency,
            dependency_name).ok(),
        "Dependency name interning failed");

    string_id member_name;

    require(
        transaction.strings().intern(
            "member_" + std::string{name},
            member_name).ok(),
        "Dependency member name interning failed");

    const std::array members{
        member_build{
            member_name,
            std::nullopt,
            dependency_name,
            0,
            0
        }
    };

    require(
        replacement.define_members(
            type,
            members,
            {}).ok(),
        "Dependency member definition failed");
}

void dependency_chain_gate() {
    graph_manager manager;

    require(
        manager.initialize().ok(),
        "Dependency manager init failed");

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        define_empty_aggregate(
            transaction,
            0,
            "T0");
        define_empty_aggregate(
            transaction,
            1,
            "T1",
            "T0");
        define_empty_aggregate(
            transaction,
            2,
            "T2",
            "T1");

        require(
            transaction.commit().ok(),
            "Dependency G0 failed");
    }

    auto transaction =
        manager.begin_build(graph_build_mode::incremental);

    define_empty_aggregate(
        transaction,
        0,
        "T0");

    require(
        access::prepare(transaction).ok(),
        "Dependency incremental prepare failed");

    require(
        rc_v2_01a::dependency_chain_gate(
            access::graph_telemetry(transaction)),
        "Dependency closure was not exactly 3 types / 2 edges");

    access::publish(transaction);
}

void print_header() {
    std::cout
        << "types,scenario,total_ms,setup_ms,prepare_ms,publish_ms,"
        << "changed_sources,changed_entities,changed_types,"
        << "validation_visited_types,validation_visited_type_refs,"
        << "validation_dependency_edges,graph_reallocated,"
        << "contribution_reallocated,graph_relocation_bytes,"
        << "contribution_relocation_bytes\n";
}

void print_row(const row& value) {
    std::cout
        << value.types << ','
        << value.scenario << ','
        << std::fixed << std::setprecision(6)
        << value.total_ms << ','
        << value.setup_ms << ','
        << value.prepare_ms << ','
        << value.publish_ms << ','
        << value.graph.changed_sources << ','
        << value.graph.changed_entities << ','
        << value.graph.changed_types << ','
        << value.graph.validation_visited_types << ','
        << value.graph.validation_visited_type_refs << ','
        << value.graph.validation_dependency_edges << ','
        << (rc_v2_01a::graph_reallocated(
                value.graph) ? 1 : 0) << ','
        << (value.contribution_reallocated ? 1 : 0) << ','
        << rc_v2_01a::graph_relocation_bytes(
                value.graph) << ','
        << value.contribution_relocation_bytes
        << '\n';
}

void run_matrix(std::size_t count) {
    graph_manager manager;

    require(
        manager.initialize().ok(),
        "Matrix manager initialization failed");

    source_id target_source;
    string_id target_name;

    const auto initial =
        g0_initial(
            manager,
            count,
            target_source,
            target_name);

    require(
        initial.graph.validation_visited_types ==
            count,
        "G0 did not validate all live types");

    require_g0_headroom(manager, count);

    const auto modify =
        g1_modify_one(
            manager,
            count,
            target_source,
            target_name);

    require_incremental_gates(modify);

    const auto remove =
        g2_remove_one(
            manager,
            count,
            target_source);

    require_incremental_gates(remove);

    string_id replacement_name;

    const auto add =
        g3_add_one(
            manager,
            count,
            target_source,
            replacement_name);

    require_incremental_gates(add);

    const auto rebuild =
        g0_rebuild_after_churn(
            manager,
            count,
            target_source,
            replacement_name);

    require(
        rebuild.graph.validation_visited_types ==
            count,
        "Rebuild did not validate all live types");

    print_row(initial);
    print_row(modify);
    print_row(remove);
    print_row(add);
    print_row(rebuild);
}

} // namespace

int main() {
    fail_closed_gate();
    dependency_chain_gate();

    print_header();

    for (const auto count :
         std::array<std::size_t, 5>{
             128,
             512,
             2048,
             8192,
             32768}) {
        run_matrix(count);
    }

    std::cout << "RC-V2-01A PASS\n";
    return 0;
}
