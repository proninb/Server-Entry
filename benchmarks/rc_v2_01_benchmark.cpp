#include "../server_entry/project/graph/graph_build_transaction_test_access.hpp"
#include "../server_entry/tests/rc_v2_01a_gates.hpp"
#include "../server_entry/metrics/source_acquisition_telemetry.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    double setup_replace_source_ms = 0;
    double setup_declare_named_ms = 0;
    graph_build_transaction_timing timing{};
    graph_storage_prepare_telemetry graph{};
    string_registry_storage_snapshot strings_before{};
    string_registry_storage_snapshot strings_after{};
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

double milliseconds(std::uint64_t nanoseconds) noexcept {
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}

std::uint64_t prepare_accounted_ns(
    const graph_build_transaction_timing& timing) noexcept {

    return
        timing.source_prepare_ns +
        timing.string_prepare_ns +
        timing.graph_prepare_ns +
        timing.string_retention_ns +
        timing.string_compaction_ns +
        timing.contribution_prepare_ns;
}

std::filesystem::path source_path(std::size_t index) {
    return
        std::filesystem::temp_directory_path() /
        "cw_rc_v2_01_benchmark_sources" /
        ("source_" + std::to_string(index) + ".cpp");
}

std::vector<source_id> prepare_physical_sources(
    graph_build_transaction& transaction,
    std::size_t count) {

    const auto root =
        std::filesystem::temp_directory_path() /
        "cw_rc_v2_01_benchmark_sources";

    std::error_code error;
    std::filesystem::create_directories(root, error);

    require(!error, "Benchmark Source directory creation failed");

    source_acquisition_telemetry telemetry{
        metrics_mode::off
    };

    std::vector<source_id> sources;
    sources.reserve(count);

    for (std::size_t index = 0;
         index < count;
         ++index) {
        const auto path = source_path(index);

        if (!std::filesystem::exists(path, error)) {
            require(
                !error,
                "Benchmark Source existence check failed");

            std::ofstream output{
                path,
                std::ios::binary | std::ios::trunc
            };

            require(
                static_cast<bool>(output),
                "Benchmark Source creation failed");
        }
        else {
            require(
                !error,
                "Benchmark Source existence check failed");
        }

        source_id source;

        require(
            transaction.sources().resolve(
                path,
                project_item_role::source,
                source).ok(),
            "Physical Source resolution failed");

        require(
            transaction.sources().acquire(
                source,
                telemetry).ok(),
            "Physical Source acquisition failed");

        sources.push_back(source);
    }

    return sources;
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
    source_id source,
    std::string_view spelling) {

    prepared_type result;
    result.source = source;

    require(
        transaction.strings().bind(
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

    type_id entity;
    type_handle type;

    require(
        replacement.add_named_enum(
            name,
            data,
            entity,
            type).ok(),
        "Enum materialization failed");
}

void replace_with_opaque_enum_scoped(
    graph_build_transaction& transaction,
    source_id source,
    string_id name,
    bool scoped) {

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            source,
            replacement).ok(),
        "Scoped enum Source replacement failed");

    const enum_build_data data{
        enum_definition_state::opaque,
        scoped,
        builtin_type::integer,
        {}
    };

    type_id entity;
    type_handle type;

    require(
        replacement.add_named_enum(
            name,
            data,
            entity,
            type).ok(),
        "Scoped enum materialization failed");
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
    source_contribution_storage_snapshot contribution_before,
    string_registry_storage_snapshot strings_before) {

    const auto prepared =
        access::prepare(transaction);

    const auto prepare_done =
        clock_type::now();

    require(prepared.ok(), "Transaction prepare failed");

    const auto timing =
        transaction.timing();

    const auto graph_telemetry =
        access::graph_telemetry(transaction);

    const auto strings_after_prepare =
        access::string_storage(manager);

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
    result.timing = timing;
    result.graph = graph_telemetry;
    result.strings_before = strings_before;
    result.strings_after = strings_after_prepare;
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

status reserve_string_capacity(
    string_registry_update& strings,
    std::size_t count) noexcept {

    // Benchmark-generated names are short. Reserve a deliberately conservative
    // byte upper bound so RC01 still measures canonical work, not arena growth.
    constexpr std::size_t generated_name_capacity = 128;

    return strings.reserve_bindings(
        count,
        count * generated_name_capacity);
}
row g0_initial(
    graph_manager& manager,
    std::size_t count,
    source_id& target_source,
    string_id& target_name) {

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    const auto sources =
        prepare_physical_sources(
            transaction,
            count);

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);

    // Physical Source setup is deliberately outside the measured interval.
    // RC-V2-01 measures canonical String/Entity/Graph construction only.
    const auto started = clock_type::now();

    require(
        reserve_string_capacity(transaction.strings(), count)
            .ok(),
        "G0 String Registry bulk reserve failed");

    for (std::size_t index = 0;
         index < count;
         ++index) {
        const auto name = type_name(index);
        const auto prepared =
            prepare_source(
                transaction,
                sources[index],
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
        contribution_before,
        strings_before);
}

row g1_modify_one(
    graph_manager& manager,
    std::size_t count,
    source_id target_source,
    string_id target_name) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
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
        contribution_before,
        strings_before);
}

row g2_remove_one(
    graph_manager& manager,
    std::size_t count,
    source_id target_source) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
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
        contribution_before,
        strings_before);
}

row g3_add_one(
    graph_manager& manager,
    std::size_t count,
    source_id target_source,
    string_id& replacement_name) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started = clock_type::now();

    auto transaction =
        manager.begin_build(graph_build_mode::incremental);

    require(
        transaction.strings().bind(
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
        contribution_before,
        strings_before);
}

row g0_rebuild_after_churn(
    graph_manager& manager,
    std::size_t count,
    source_id target_source,
    string_id replacement_name) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started = clock_type::now();

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    require(
        reserve_string_capacity(transaction.strings(), count)
            .ok(),
        "Rebuild String Registry bulk reserve failed");

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
                transaction.strings().bind(
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
        contribution_before,
        strings_before);
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

        const auto sources =
            prepare_physical_sources(
                transaction,
                1);

        const auto prepared =
            prepare_source(
                transaction,
                sources[0],
                "Base");

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
        transaction.strings().bind(
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
    source_id source,
    std::string_view name,
    std::string_view dependency = {}) {

    const auto prepared =
        prepare_source(
            transaction,
            source,
            name);

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            prepared.source,
            replacement).ok(),
        "Dependency Source replacement failed");

    type_id entity;
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
        transaction.strings().bind(
            dependency,
            dependency_name).ok(),
        "Dependency name interning failed");

    string_id member_name;

    require(
        transaction.strings().bind(
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

        const auto sources =
            prepare_physical_sources(
                transaction,
                3);

        define_empty_aggregate(
            transaction,
            sources[0],
            "T0");
        define_empty_aggregate(
            transaction,
            sources[1],
            "T1",
            "T0");
        define_empty_aggregate(
            transaction,
            sources[2],
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
        source_id{1},
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

void print_row(const row& value);

constexpr std::size_t dependency_scaling_max_depth = 256;

struct dependency_scaling_baseline {
    std::vector<source_id> sources;
};

const char* dependency_scaling_scenario(
    std::size_t depth) noexcept {

    switch (depth) {
    case 1:
        return "dependency_d1";
    case 4:
        return "dependency_d4";
    case 16:
        return "dependency_d16";
    case 64:
        return "dependency_d64";
    case 256:
        return "dependency_d256";
    default:
        return "dependency_invalid";
    }
}

dependency_scaling_baseline build_dependency_scaling_baseline(
    graph_manager& manager,
    std::size_t count) {

    require(
        count >= dependency_scaling_max_depth,
        "RC-V2-04 N smaller than dependency chain");

    auto transaction =
        manager.begin_build(
            graph_build_mode::rebuild);

    auto sources =
        prepare_physical_sources(
            transaction,
            count);

    require(
        reserve_string_capacity(transaction.strings(),
                count +
                dependency_scaling_max_depth)
            .ok(),
        "RC-V2-04 G0 String reserve failed");

    for (std::size_t index = 0;
         index < count;
         ++index) {
        const auto name =
            type_name(index);

        if (index != 0 &&
            index < dependency_scaling_max_depth) {
            const auto dependency =
                type_name(index - 1);

            define_empty_aggregate(
                transaction,
                sources[index],
                name,
                dependency);
        }
        else {
            define_empty_aggregate(
                transaction,
                sources[index],
                name);
        }
    }

    require(
        transaction.commit().ok(),
        "RC-V2-04 G0 dependency baseline failed");

    require_g0_headroom(
        manager,
        count);

    return {
        std::move(sources)
    };
}

row dependency_scaling_case(
    graph_manager& manager,
    std::size_t count,
    std::size_t depth,
    const dependency_scaling_baseline& baseline) {

    require(
        depth != 0 &&
        depth <= dependency_scaling_max_depth,
        "RC-V2-04 invalid dependency depth");

    const auto root =
        dependency_scaling_max_depth - depth;

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started =
        clock_type::now();

    auto transaction =
        manager.begin_build(
            graph_build_mode::incremental);

    const auto name =
        type_name(root);

    if (root == 0) {
        define_empty_aggregate(
            transaction,
            baseline.sources[root],
            name);
    }
    else {
        const auto dependency =
            type_name(root - 1);

        define_empty_aggregate(
            transaction,
            baseline.sources[root],
            name,
            dependency);
    }

    const auto setup_done =
        clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        dependency_scaling_scenario(depth),
        started,
        setup_done,
        contribution_before,
        strings_before);
}

void require_dependency_scaling_gates(
    const row& value,
    std::size_t depth) {

    const auto& graph = value.graph;

    require(
        graph.changed_sources == 1,
        "RC-V2-04 changed_sources != 1");

    require(
        graph.changed_entities == 1,
        "RC-V2-04 changed_entities != 1");

    require(
        graph.changed_types == 1,
        "RC-V2-04 changed_types != 1");

    require(
        graph.validation_visited_types == depth,
        "RC-V2-04 validation closure != D");

    require(
        graph.validation_dependency_edges ==
            depth - 1,
        "RC-V2-04 dependency edges != D-1");

    const auto expected_type_refs =
        depth == dependency_scaling_max_depth
            ? depth - 1
            : depth;

    require(
        graph.validation_visited_type_refs ==
            expected_type_refs,
        "RC-V2-04 TypeRef visits do not match chain");

    require(
        !rc_v2_01a::graph_reallocated(graph),
        "RC-V2-04 committed Graph storage reallocated");

    require(
        !value.contribution_reallocated,
        "RC-V2-04 SourceContribution storage reallocated");

    require(
        value.strings_before.records_data ==
            value.strings_after.records_data,
        "RC-V2-04 String records relocated");

    require(
        value.strings_before.lookup_bucket_count ==
            value.strings_after.lookup_bucket_count,
        "RC-V2-04 String lookup rehashed");
}

void run_dependency_scaling_matrix() {

    for (const auto count :
         std::array<std::size_t, 2>{
             8192,
             32768}) {
        graph_manager manager;

        require(
            manager.initialize().ok(),
            "RC-V2-04 manager initialization failed");

        const auto baseline =
            build_dependency_scaling_baseline(
                manager,
                count);

        for (const auto depth :
             std::array<std::size_t, 5>{
                 1,
                 4,
                 16,
                 64,
                 256}) {
            const auto value =
                dependency_scaling_case(
                    manager,
                    count,
                    depth,
                    baseline);

            require_dependency_scaling_gates(
                value,
                depth);

            print_row(value);
        }
    }
}

constexpr std::size_t multi_root_count = 256;
constexpr std::size_t multi_root_shared_count = 256;
constexpr std::size_t multi_root_first_shared = multi_root_count;
constexpr std::size_t multi_root_total_graph_types =
    multi_root_count + multi_root_shared_count;

struct multi_root_baseline {
    std::vector<source_id> sources;
    std::vector<string_id> root_names;
};

const char* multi_root_scenario(
    std::size_t roots) noexcept {

    switch (roots) {
    case 1:
        return "multi_root_k1";
    case 4:
        return "multi_root_k4";
    case 16:
        return "multi_root_k16";
    case 64:
        return "multi_root_k64";
    case 256:
        return "multi_root_k256";
    default:
        return "multi_root_invalid";
    }
}

void define_multi_root_hub(
    graph_build_transaction& transaction,
    source_id source,
    std::string_view name) {

    const auto prepared =
        prepare_source(
            transaction,
            source,
            name);

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            prepared.source,
            replacement).ok(),
        "RC-V2-05 hub Source replacement failed");

    type_id entity;
    type_handle type;

    require(
        replacement.add_named_type(
            prepared.name,
            aggregate_definition_state::defined,
            entity,
            type).ok(),
        "RC-V2-05 hub aggregate creation failed");

    std::vector<member_build> members;
    members.reserve(multi_root_count);

    for (std::size_t root = 0;
         root < multi_root_count;
         ++root) {
        string_id dependency_name;

        require(
            transaction.strings().bind(
                type_name(root),
                dependency_name).ok(),
            "RC-V2-05 hub dependency name failed");

        string_id member_name;

        require(
            transaction.strings().bind(
                "root_member_" +
                    std::to_string(root),
                member_name).ok(),
            "RC-V2-05 hub member name failed");

        members.push_back(
            member_build{
                member_name,
                std::nullopt,
                dependency_name,
                0,
                0
            });
    }

    require(
        replacement.define_members(
            type,
            members,
            {}).ok(),
        "RC-V2-05 hub member definition failed");
}

multi_root_baseline build_multi_root_baseline(
    graph_manager& manager,
    std::size_t count) {

    require(
        count >= multi_root_total_graph_types,
        "RC-V2-05 N smaller than topology");

    auto transaction =
        manager.begin_build(
            graph_build_mode::rebuild);

    auto sources =
        prepare_physical_sources(
            transaction,
            count);

    require(
        reserve_string_capacity(transaction.strings(),
                count +
                multi_root_count +
                multi_root_shared_count)
            .ok(),
        "RC-V2-05 G0 String reserve failed");

    std::vector<string_id> root_names;
    root_names.resize(multi_root_count);

    for (std::size_t root = 0;
         root < multi_root_count;
         ++root) {
        require(
            transaction.strings().bind(
                type_name(root),
                root_names[root]).ok(),
            "RC-V2-05 root String interning failed");

        replace_with_opaque_enum_scoped(
            transaction,
            sources[root],
            root_names[root],
            false);
    }

    define_multi_root_hub(
        transaction,
        sources[multi_root_first_shared],
        type_name(multi_root_first_shared));

    for (std::size_t index =
             multi_root_first_shared + 1;
         index < multi_root_total_graph_types;
         ++index) {
        define_empty_aggregate(
            transaction,
            sources[index],
            type_name(index),
            type_name(index - 1));
    }

    for (std::size_t index =
             multi_root_total_graph_types;
         index < count;
         ++index) {
        const auto prepared =
            prepare_source(
                transaction,
                sources[index],
                type_name(index));

        replace_with_opaque_enum(
            transaction,
            prepared.source,
            prepared.name);
    }

    require(
        transaction.commit().ok(),
        "RC-V2-05 G0 baseline failed");

    require_g0_headroom(
        manager,
        count);

    return {
        std::move(sources),
        std::move(root_names)
    };
}

row multi_root_dependency_case(
    graph_manager& manager,
    std::size_t count,
    std::size_t roots,
    bool scoped,
    const multi_root_baseline& baseline) {

    require(
        roots != 0 &&
        roots <= multi_root_count,
        "RC-V2-05 invalid root count");

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started =
        clock_type::now();

    auto transaction =
        manager.begin_build(
            graph_build_mode::incremental);

    for (std::size_t root = 0;
         root < roots;
         ++root) {
        replace_with_opaque_enum_scoped(
            transaction,
            baseline.sources[root],
            baseline.root_names[root],
            scoped);
    }

    const auto setup_done =
        clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        multi_root_scenario(roots),
        started,
        setup_done,
        contribution_before,
        strings_before);
}

void require_multi_root_dependency_gates(
    const row& value,
    std::size_t roots) {

    const auto& graph =
        value.graph;

    require(
        graph.changed_sources == roots,
        "RC-V2-05 changed_sources != K");

    require(
        graph.changed_entities == roots,
        "RC-V2-05 changed_entities != K");

    require(
        graph.changed_types == roots,
        "RC-V2-05 changed_types != K");

    require(
        graph.validation_visited_types ==
            roots + multi_root_shared_count,
        "RC-V2-05 overlapping closure was not deduplicated");

    require(
        graph.validation_dependency_edges ==
            roots + multi_root_shared_count - 1,
        "RC-V2-05 dependency edges were revisited");

    require(
        graph.validation_visited_type_refs ==
            multi_root_count +
            multi_root_shared_count - 1,
        "RC-V2-05 TypeRefs were revisited");

    require(
        !rc_v2_01a::graph_reallocated(graph),
        "RC-V2-05 committed Graph storage reallocated");

    require(
        !value.contribution_reallocated,
        "RC-V2-05 SourceContribution storage reallocated");

    require(
        value.strings_before.records_data ==
            value.strings_after.records_data,
        "RC-V2-05 String records relocated");

    require(
        value.strings_before.lookup_bucket_count ==
            value.strings_after.lookup_bucket_count,
        "RC-V2-05 String lookup rehashed");
}

void run_multi_root_dependency_matrix() {

    for (const auto count :
         std::array<std::size_t, 2>{
             8192,
             32768}) {
        graph_manager manager;

        require(
            manager.initialize().ok(),
            "RC-V2-05 manager initialization failed");

        const auto baseline =
            build_multi_root_baseline(
                manager,
                count);

        bool scoped = true;

        // Descending K lets one baseline serve the whole matrix while every
        // selected root changes semantic state in every case.
        for (const auto roots :
             std::array<std::size_t, 5>{
                 256,
                 64,
                 16,
                 4,
                 1}) {
            const auto value =
                multi_root_dependency_case(
                    manager,
                    count,
                    roots,
                    scoped,
                    baseline);

            require_multi_root_dependency_gates(
                value,
                roots);

            print_row(value);
            scoped = !scoped;
        }
    }
}

constexpr std::size_t edge_scaling_dependents = 256;
constexpr std::size_t edge_scaling_region_size =
    edge_scaling_dependents + 1;

constexpr std::array<std::size_t, 5>
    edge_scaling_fanins{
        0,
        1,
        3,
        15,
        63
    };

struct edge_scaling_region {
    source_id root_source{};
    string_id root_name{};
    std::size_t fanin = 0;
};

struct edge_scaling_baseline {
    std::vector<edge_scaling_region> regions;
};

std::size_t edge_scaling_expected_edges(
    std::size_t fanin) noexcept {

    std::size_t edges =
        edge_scaling_dependents;

    for (std::size_t index = 0;
         index < edge_scaling_dependents;
         ++index) {
        edges +=
            std::min(
                fanin,
                index);
    }

    return edges;
}

const char* edge_scaling_scenario(
    std::size_t fanin) noexcept {

    switch (fanin) {
    case 0:
        return "edge_f0";
    case 1:
        return "edge_f1";
    case 3:
        return "edge_f3";
    case 15:
        return "edge_f15";
    case 63:
        return "edge_f63";
    default:
        return "edge_invalid";
    }
}

void define_edge_scaling_aggregate(
    graph_build_transaction& transaction,
    source_id source,
    std::size_t type_index,
    std::size_t root_index,
    std::size_t dependent_position,
    std::size_t fanin) {

    const auto prepared =
        prepare_source(
            transaction,
            source,
            type_name(type_index));

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            prepared.source,
            replacement).ok(),
        "RC-V2-06 aggregate Source replacement failed");

    type_id entity;
    type_handle type;

    require(
        replacement.add_named_type(
            prepared.name,
            aggregate_definition_state::defined,
            entity,
            type).ok(),
        "RC-V2-06 aggregate creation failed");

    const auto extra =
        std::min(
            fanin,
            dependent_position);

    std::vector<member_build> members;
    members.reserve(extra + 1);

    const auto append_member =
        [&](std::size_t dependency_index,
            std::size_t member_index) {
            string_id dependency_name;

            require(
                transaction.strings().bind(
                    type_name(dependency_index),
                    dependency_name).ok(),
                "RC-V2-06 dependency name interning failed");

            string_id member_name;

            require(
                transaction.strings().bind(
                    "edge_member_" +
                        std::to_string(type_index) +
                        "_" +
                        std::to_string(member_index),
                    member_name).ok(),
                "RC-V2-06 member name interning failed");

            members.push_back(
                member_build{
                    member_name,
                    std::nullopt,
                    dependency_name,
                    0,
                    0
                });
        };

    append_member(
        root_index,
        0);

    for (std::size_t offset = 0;
         offset < extra;
         ++offset) {
        const auto dependency_index =
            type_index - 1 - offset;

        append_member(
            dependency_index,
            offset + 1);
    }

    require(
        replacement.define_members(
            type,
            members,
            {}).ok(),
        "RC-V2-06 aggregate member definition failed");
}

edge_scaling_baseline build_edge_scaling_baseline(
    graph_manager& manager,
    std::size_t count) {

    const auto topology_types =
        edge_scaling_region_size *
        edge_scaling_fanins.size();

    require(
        count >= topology_types,
        "RC-V2-06 N smaller than topology");

    auto transaction =
        manager.begin_build(
            graph_build_mode::rebuild);

    auto sources =
        prepare_physical_sources(
            transaction,
            count);

    std::size_t topology_edges = 0;

    for (const auto fanin :
         edge_scaling_fanins) {
        topology_edges +=
            edge_scaling_expected_edges(
                fanin);
    }

    require(
        reserve_string_capacity(transaction.strings(),
                count +
                topology_edges)
            .ok(),
        "RC-V2-06 G0 String reserve failed");

    edge_scaling_baseline baseline;
    baseline.regions.reserve(
        edge_scaling_fanins.size());

    for (std::size_t region_index = 0;
         region_index <
            edge_scaling_fanins.size();
         ++region_index) {
        const auto fanin =
            edge_scaling_fanins[
                region_index];

        const auto root_index =
            region_index *
            edge_scaling_region_size;

        string_id root_name;

        require(
            transaction.strings().bind(
                type_name(root_index),
                root_name).ok(),
            "RC-V2-06 root String interning failed");

        replace_with_opaque_enum_scoped(
            transaction,
            sources[root_index],
            root_name,
            false);

        baseline.regions.push_back(
            edge_scaling_region{
                sources[root_index],
                root_name,
                fanin
            });

        for (std::size_t position = 0;
             position <
                edge_scaling_dependents;
             ++position) {
            const auto type_index =
                root_index +
                1 +
                position;

            define_edge_scaling_aggregate(
                transaction,
                sources[type_index],
                type_index,
                root_index,
                position,
                fanin);
        }
    }

    for (std::size_t index =
             topology_types;
         index < count;
         ++index) {
        const auto prepared =
            prepare_source(
                transaction,
                sources[index],
                type_name(index));

        replace_with_opaque_enum(
            transaction,
            prepared.source,
            prepared.name);
    }

    require(
        transaction.commit().ok(),
        "RC-V2-06 G0 baseline failed");

    require_g0_headroom(
        manager,
        count);

    return baseline;
}

row edge_scaling_case(
    graph_manager& manager,
    std::size_t count,
    const edge_scaling_region& region) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started =
        clock_type::now();

    auto transaction =
        manager.begin_build(
            graph_build_mode::incremental);

    replace_with_opaque_enum_scoped(
        transaction,
        region.root_source,
        region.root_name,
        true);

    const auto setup_done =
        clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        edge_scaling_scenario(
            region.fanin),
        started,
        setup_done,
        contribution_before,
        strings_before);
}

void require_edge_scaling_gates(
    const row& value,
    std::size_t fanin) {

    const auto& graph =
        value.graph;

    const auto expected_edges =
        edge_scaling_expected_edges(
            fanin);

    require(
        graph.changed_sources == 1,
        "RC-V2-06 changed_sources != 1");

    require(
        graph.changed_entities == 1,
        "RC-V2-06 changed_entities != 1");

    require(
        graph.changed_types == 1,
        "RC-V2-06 changed_types != 1");

    require(
        graph.validation_visited_types ==
            edge_scaling_region_size,
        "RC-V2-06 D changed with E");

    require(
        graph.validation_dependency_edges ==
            expected_edges,
        "RC-V2-06 dependency edges != expected E");

    require(
        graph.validation_visited_type_refs ==
            expected_edges,
        "RC-V2-06 TypeRef visits != expected E");

    require(
        !rc_v2_01a::graph_reallocated(graph),
        "RC-V2-06 committed Graph storage reallocated");

    require(
        !value.contribution_reallocated,
        "RC-V2-06 SourceContribution storage reallocated");

    require(
        value.strings_before.records_data ==
            value.strings_after.records_data,
        "RC-V2-06 String records relocated");

    require(
        value.strings_before.lookup_bucket_count ==
            value.strings_after.lookup_bucket_count,
        "RC-V2-06 String lookup rehashed");
}

void run_edge_scaling_matrix() {

    for (const auto count :
         std::array<std::size_t, 2>{
             8192,
             32768}) {
        graph_manager manager;

        require(
            manager.initialize().ok(),
            "RC-V2-06 manager initialization failed");

        const auto baseline =
            build_edge_scaling_baseline(
                manager,
                count);

        for (const auto& region :
             baseline.regions) {
            const auto value =
                edge_scaling_case(
                    manager,
                    count,
                    region);

            require_edge_scaling_gates(
                value,
                region.fanin);

            print_row(value);
        }
    }
}

constexpr std::size_t typeref_resolution_filler_members = 4096;
constexpr std::size_t typeref_resolution_max_refs = 256;

struct typeref_resolution_baseline {
    source_id target_source{};
    string_id target_name{};
    string_id dependency_name{};
    std::vector<string_id> member_names;
};

const char* typeref_resolution_scenario(
    std::size_t refs) noexcept {

    switch (refs) {
    case 1:
        return "typeref_r1";
    case 4:
        return "typeref_r4";
    case 16:
        return "typeref_r16";
    case 64:
        return "typeref_r64";
    case 256:
        return "typeref_r256";
    default:
        return "typeref_invalid";
    }
}

void define_typeref_filler(
    graph_build_transaction& transaction,
    source_id source,
    string_id name,
    std::vector<string_id>& retained_member_names) {

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            source,
            replacement).ok(),
        "RC-V2-07 filler Source replacement failed");

    type_id entity;
    type_handle type;

    require(
        replacement.add_named_type(
            name,
            aggregate_definition_state::defined,
            entity,
            type).ok(),
        "RC-V2-07 filler aggregate creation failed");

    std::vector<member_build> members;
    members.reserve(
        typeref_resolution_filler_members);

    retained_member_names.clear();
    retained_member_names.reserve(
        typeref_resolution_max_refs);

    for (std::size_t index = 0;
         index < typeref_resolution_filler_members;
         ++index) {
        string_id member_name;

        require(
            transaction.strings().bind(
                "typeref_member_" +
                    std::to_string(index),
                member_name).ok(),
            "RC-V2-07 filler member String interning failed");

        if (index < typeref_resolution_max_refs) {
            retained_member_names.push_back(
                member_name);
        }

        members.push_back(
            member_build{
                member_name,
                std::optional<builtin_type>{
                    builtin_type::integer
                },
                {},
                0,
                0
            });
    }

    require(
        replacement.define_members(
            type,
            members,
            {}).ok(),
        "RC-V2-07 filler member definition failed");
}

typeref_resolution_baseline
build_typeref_resolution_baseline(
    graph_manager& manager,
    std::size_t count) {

    require(
        count >= 8192,
        "RC-V2-07 N smaller than workload");

    auto transaction =
        manager.begin_build(
            graph_build_mode::rebuild);

    auto sources =
        prepare_physical_sources(
            transaction,
            count);

    require(
        reserve_string_capacity(transaction.strings(),
                count +
                typeref_resolution_filler_members +
                512)
            .ok(),
        "RC-V2-07 G0 String reserve failed");

    typeref_resolution_baseline baseline;

    require(
        transaction.strings().bind(
            type_name(0),
            baseline.dependency_name).ok(),
        "RC-V2-07 dependency name interning failed");

    replace_with_opaque_enum(
        transaction,
        sources[0],
        baseline.dependency_name);

    require(
        transaction.strings().bind(
            type_name(1),
            baseline.target_name).ok(),
        "RC-V2-07 target name interning failed");

    define_empty_aggregate(
        transaction,
        sources[1],
        type_name(1));

    baseline.target_source =
        sources[1];

    string_id filler_name;

    require(
        transaction.strings().bind(
            type_name(2),
            filler_name).ok(),
        "RC-V2-07 filler name interning failed");

    define_typeref_filler(
        transaction,
        sources[2],
        filler_name,
        baseline.member_names);

    for (std::size_t index = 3;
         index < count;
         ++index) {
        const auto prepared =
            prepare_source(
                transaction,
                sources[index],
                type_name(index));

        replace_with_opaque_enum(
            transaction,
            prepared.source,
            prepared.name);
    }

    require(
        transaction.commit().ok(),
        "RC-V2-07 G0 baseline failed");

    require(
        baseline.member_names.size() ==
            typeref_resolution_max_refs,
        "RC-V2-07 retained member name count invalid");

    require_g0_headroom(
        manager,
        count);

    const auto storage =
        access::graph_storage(manager);

    require(
        storage.canonical_types_size >= count,
        "RC-V2-07 canonical TypeRef table is not O(N)");

    require(
        storage.member_records_capacity -
            storage.member_records_size >=
            341,
        "RC-V2-07 member arena lacks incremental headroom");

    return baseline;
}

row typeref_resolution_case(
    graph_manager& manager,
    std::size_t count,
    std::size_t refs,
    const typeref_resolution_baseline& baseline) {

    require(
        refs != 0 &&
        refs <= baseline.member_names.size(),
        "RC-V2-07 invalid R");

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started =
        clock_type::now();

    auto transaction =
        manager.begin_build(
            graph_build_mode::incremental);

    graph_update::source_replacement replacement;

    require(
        transaction.graph_state().replace_source(
            baseline.target_source,
            replacement).ok(),
        "RC-V2-07 target Source replacement failed");

    type_id entity;
    type_handle type;

    require(
        replacement.add_named_type(
            baseline.target_name,
            aggregate_definition_state::defined,
            entity,
            type).ok(),
        "RC-V2-07 target aggregate creation failed");

    std::vector<member_build> members;
    members.reserve(refs);

    for (std::size_t index = 0;
         index < refs;
         ++index) {
        members.push_back(
            member_build{
                baseline.member_names[index],
                std::nullopt,
                baseline.dependency_name,
                0,
                0
            });
    }

    require(
        replacement.define_members(
            type,
            members,
            {}).ok(),
        "RC-V2-07 target member definition failed");

    const auto setup_done =
        clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        typeref_resolution_scenario(refs),
        started,
        setup_done,
        contribution_before,
        strings_before);
}

void require_typeref_resolution_gates(
    const row& value,
    std::size_t refs) {

    const auto& graph =
        value.graph;

    require(
        graph.changed_sources == 1,
        "RC-V2-07 changed_sources != 1");

    require(
        graph.changed_entities == 1,
        "RC-V2-07 changed_entities != 1");

    require(
        graph.changed_types == 1,
        "RC-V2-07 changed_types != 1");

    require(
        graph.pending_resolution_types == 1,
        "RC-V2-07 pending resolution touched != 1 Type");

    require(
        graph.pending_resolution_members == refs,
        "RC-V2-07 pending resolution members != R");

    require(
        graph.pending_resolution_modifiers == 0,
        "RC-V2-07 unexpected modifier resolution");

    require(
        graph.validation_visited_types == 1,
        "RC-V2-07 validation visited != 1 Type");

    require(
        graph.validation_visited_type_refs == refs,
        "RC-V2-07 validation TypeRefs != R");

    require(
        graph.validation_dependency_edges == 0,
        "RC-V2-07 unexpected reverse dependency traversal");

    require(
        graph.canonical_types.size_before >=
            value.types,
        "RC-V2-07 global canonical table is too small");

    require(
        graph.canonical_types.size_after ==
            graph.canonical_types.size_before,
        "RC-V2-07 incremental resolution changed canonical table size");

    require(
        !rc_v2_01a::graph_reallocated(graph),
        "RC-V2-07 committed Graph storage reallocated");

    require(
        !value.contribution_reallocated,
        "RC-V2-07 SourceContribution storage reallocated");

    require(
        value.strings_before.records_data ==
            value.strings_after.records_data,
        "RC-V2-07 String records relocated");

    require(
        value.strings_before.lookup_bucket_count ==
            value.strings_after.lookup_bucket_count,
        "RC-V2-07 String lookup rehashed");
}

void run_typeref_resolution_matrix() {

    for (const auto count :
         std::array<std::size_t, 2>{
             8192,
             32768}) {
        graph_manager manager;

        require(
            manager.initialize().ok(),
            "RC-V2-07 manager initialization failed");

        const auto baseline =
            build_typeref_resolution_baseline(
                manager,
                count);

        for (const auto refs :
             std::array<std::size_t, 5>{
                 1,
                 4,
                 16,
                 64,
                 256}) {
            const auto value =
                typeref_resolution_case(
                    manager,
                    count,
                    refs,
                    baseline);

            require_typeref_resolution_gates(
                value,
                refs);

            print_row(value);
        }
    }
}

void print_header() {
    std::cout
        << "types,scenario,total_ms,setup_ms,prepare_ms,publish_ms,"
        << "setup_replace_source_ms,setup_declare_named_ms,"
        << "prepare_accounted_ms,source_prepare_ms,string_prepare_ms,"
        << "graph_prepare_ms,string_retention_ms,string_compaction_ms,"
        << "contribution_prepare_ms,"
        << "graph_pending_member_resolution_ms,"
        << "graph_live_typeref_validation_ms,"
        << "graph_canonical_typeref_rebuild_ms,"
        << "graph_string_validation_ms,"
        << "graph_definition_scan_ms,"
        << "graph_definition_materialization_ms,"
        << "graph_rebuild_storage_ms,"
        << "graph_dependency_index_ms,"
        << "graph_final_prepare_ms,"
        << "pending_resolution_types,"
        << "pending_resolution_members,"
        << "pending_resolution_modifiers,"
        << "canonical_types_size_before,"
        << "canonical_types_size_after,"
        << "string_records_size_before,string_records_size_after,"
        << "string_records_capacity_before,string_records_capacity_after,"
        << "string_records_reallocated,string_records_relocation_bytes,"
        << "string_lookup_size_before,string_lookup_size_after,"
        << "string_lookup_buckets_before,string_lookup_buckets_after,"
        << "string_lookup_rehashed,string_lookup_entries_rehashed,"
        << "changed_sources,changed_entities,changed_types,"
        << "validation_visited_types,validation_visited_type_refs,"
        << "validation_dependency_edges,graph_reallocated,"
        << "contribution_reallocated,graph_relocation_bytes,"
        << "contribution_relocation_bytes\n";
}

void print_row(const row& value) {
    const auto string_records_reallocated =
        value.strings_before.records_data !=
        value.strings_after.records_data;

    const auto string_records_relocation_bytes =
        string_records_reallocated
            ? value.strings_before.records_size *
                sizeof(const void*)
            : 0;

    const auto string_lookup_rehashed =
        value.strings_before.lookup_bucket_count !=
        value.strings_after.lookup_bucket_count;

    const auto string_lookup_entries_rehashed =
        string_lookup_rehashed
            ? value.strings_before.lookup_size
            : 0;

    const auto& timing = value.timing;

    std::cout
        << value.types << ','
        << value.scenario << ','
        << std::fixed << std::setprecision(6)
        << value.total_ms << ','
        << value.setup_ms << ','
        << value.prepare_ms << ','
        << value.publish_ms << ','
        << value.setup_replace_source_ms << ','
        << value.setup_declare_named_ms << ','
        << milliseconds(prepare_accounted_ns(timing)) << ','
        << milliseconds(timing.source_prepare_ns) << ','
        << milliseconds(timing.string_prepare_ns) << ','
        << milliseconds(timing.graph_prepare_ns) << ','
        << milliseconds(timing.string_retention_ns) << ','
        << milliseconds(timing.string_compaction_ns) << ','
        << milliseconds(timing.contribution_prepare_ns) << ','
        << milliseconds(timing.graph_pending_member_resolution_ns) << ','
        << milliseconds(timing.graph_live_typeref_validation_ns) << ','
        << milliseconds(timing.graph_canonical_typeref_rebuild_ns) << ','
        << milliseconds(timing.graph_string_validation_ns) << ','
        << milliseconds(timing.graph_definition_scan_ns) << ','
        << milliseconds(timing.graph_definition_materialization_ns) << ','
        << milliseconds(timing.graph_rebuild_storage_ns) << ','
        << milliseconds(timing.graph_dependency_index_ns) << ','
        << milliseconds(timing.graph_final_prepare_ns) << ','
        << value.graph.pending_resolution_types << ','
        << value.graph.pending_resolution_members << ','
        << value.graph.pending_resolution_modifiers << ','
        << value.graph.canonical_types.size_before << ','
        << value.graph.canonical_types.size_after << ','
        << value.strings_before.records_size << ','
        << value.strings_after.records_size << ','
        << value.strings_before.records_capacity << ','
        << value.strings_after.records_capacity << ','
        << (string_records_reallocated ? 1 : 0) << ','
        << string_records_relocation_bytes << ','
        << value.strings_before.lookup_size << ','
        << value.strings_after.lookup_size << ','
        << value.strings_before.lookup_bucket_count << ','
        << value.strings_after.lookup_bucket_count << ','
        << (string_lookup_rehashed ? 1 : 0) << ','
        << string_lookup_entries_rehashed << ','
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

struct k_baseline {
    std::vector<source_id> sources;
    std::vector<string_id> names;
};

std::size_t k_source_index(
    std::size_t count,
    std::size_t k,
    std::size_t position) {

    require(
        k != 0 &&
        k <= count &&
        position < k,
        "Invalid RC-V2-03 K selection");

    return (position * count) / k;
}

k_baseline build_k_baseline(
    graph_manager& manager,
    std::size_t count) {

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    auto sources =
        prepare_physical_sources(
            transaction,
            count);

    require(
        reserve_string_capacity(transaction.strings(), count)
            .ok(),
        "RC-V2-03 G0 String bulk reserve failed");

    std::vector<string_id> names;
    names.resize(count);

    for (std::size_t index = 0;
         index < count;
         ++index) {
        require(
            transaction.strings().bind(
                type_name(index),
                names[index]).ok(),
            "RC-V2-03 G0 String interning failed");

        replace_with_opaque_enum(
            transaction,
            sources[index],
            names[index]);
    }

    require(
        transaction.commit().ok(),
        "RC-V2-03 G0 commit failed");

    require_g0_headroom(manager, count);

    return {
        std::move(sources),
        std::move(names)
    };
}

row k_modify(
    graph_manager& manager,
    std::size_t count,
    std::size_t k,
    const k_baseline& baseline) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started =
        clock_type::now();

    auto transaction =
        manager.begin_build(
            graph_build_mode::incremental);

    for (std::size_t position = 0;
         position < k;
         ++position) {
        const auto index =
            k_source_index(
                count,
                k,
                position);

        replace_with_opaque_enum(
            transaction,
            baseline.sources[index],
            baseline.names[index]);
    }

    const auto setup_done =
        clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        "k_modify",
        started,
        setup_done,
        contribution_before,
        strings_before);
}

row k_remove(
    graph_manager& manager,
    std::size_t count,
    std::size_t k,
    const k_baseline& baseline) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started =
        clock_type::now();

    auto transaction =
        manager.begin_build(
            graph_build_mode::incremental);

    for (std::size_t position = 0;
         position < k;
         ++position) {
        const auto index =
            k_source_index(
                count,
                k,
                position);

        replace_empty(
            transaction,
            baseline.sources[index]);
    }

    const auto setup_done =
        clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        "k_remove",
        started,
        setup_done,
        contribution_before,
        strings_before);
}

row k_add(
    graph_manager& manager,
    std::size_t count,
    std::size_t k,
    const k_baseline& baseline) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started =
        clock_type::now();

    auto transaction =
        manager.begin_build(
            graph_build_mode::incremental);

    require(
        reserve_string_capacity(transaction.strings(), k)
            .ok(),
        "RC-V2-03 incremental String bulk reserve failed");

    for (std::size_t position = 0;
         position < k;
         ++position) {
        const auto index =
            k_source_index(
                count,
                k,
                position);

        string_id replacement_name;

        require(
            transaction.strings().bind(
                "K_replacement_" +
                    std::to_string(count) + "_" +
                    std::to_string(k) + "_" +
                    std::to_string(position),
                replacement_name).ok(),
            "RC-V2-03 replacement String interning failed");

        replace_with_opaque_enum(
            transaction,
            baseline.sources[index],
            replacement_name);
    }

    const auto setup_done =
        clock_type::now();

    return finish_row(
        manager,
        transaction,
        count,
        "k_add",
        started,
        setup_done,
        contribution_before,
        strings_before);
}

void require_k_noop_gates(
    const row& value,
    std::size_t k) {

    const auto& graph = value.graph;

    require(
        graph.changed_sources == k,
        "RC-V2-03G changed_sources != K");

    require(
        graph.changed_entities == 0,
        "RC-V2-03G no-op changed Entities");

    require(
        graph.changed_types == 0,
        "RC-V2-03G no-op changed Types");

    require(
        graph.validation_visited_types == 0,
        "RC-V2-03G no-op validation visited Types");

    require(
        graph.validation_visited_type_refs == 0 &&
        graph.validation_dependency_edges == 0,
        "RC-V2-03G no-op dependency work");

    require(
        !rc_v2_01a::graph_reallocated(graph),
        "RC-V2-03G Graph storage reallocated");

    require(
        !value.contribution_reallocated,
        "RC-V2-03G SourceContribution storage reallocated");

    require(
        value.strings_before.records_data ==
            value.strings_after.records_data,
        "RC-V2-03G String records relocated");

    require(
        value.strings_before.lookup_bucket_count ==
            value.strings_after.lookup_bucket_count,
        "RC-V2-03G String lookup rehashed");
}

void require_k_gates(
    const row& value,
    std::size_t k) {

    const auto& graph = value.graph;

    require(
        graph.changed_sources == k,
        "RC-V2-03 changed_sources != K");

    require(
        graph.changed_entities == k,
        "RC-V2-03 changed_entities != K");

    require(
        graph.changed_types == k,
        "RC-V2-03 changed_types != K");

    require(
        graph.validation_visited_types == k,
        "RC-V2-03 validation_visited_types != K");

    require(
        graph.validation_visited_type_refs == 0,
        "RC-V2-03 unexpected TypeRef validation work");

    require(
        graph.validation_dependency_edges == 0,
        "RC-V2-03 unexpected dependency closure");

    require(
        !rc_v2_01a::graph_reallocated(graph),
        "RC-V2-03 committed Graph storage reallocated");

    require(
        !value.contribution_reallocated,
        "RC-V2-03 SourceContribution storage reallocated");

    require(
        value.strings_before.records_data ==
            value.strings_after.records_data,
        "RC-V2-03 String records relocated");

    require(
        value.strings_before.lookup_bucket_count ==
            value.strings_after.lookup_bucket_count,
        "RC-V2-03 String lookup rehashed");
}

void run_k_matrix(
    std::size_t count,
    std::size_t k) {

    graph_manager manager;

    require(
        manager.initialize().ok(),
        "RC-V2-03 manager initialization failed");

    const auto baseline =
        build_k_baseline(
            manager,
            count);

    const auto modify =
        k_modify(
            manager,
            count,
            k,
            baseline);

    require_k_noop_gates(modify, k);

    const auto remove =
        k_remove(
            manager,
            count,
            k,
            baseline);

    require_k_gates(remove, k);

    const auto add =
        k_add(
            manager,
            count,
            k,
            baseline);

    require_k_gates(add, k);

    print_row(modify);
    print_row(remove);
    print_row(add);
}
enum class k_locality_mode : std::uint8_t {
    clustered,
    spread
};

std::size_t k_locality_index(
    std::size_t count,
    std::size_t k,
    std::size_t position,
    k_locality_mode mode) {

    if (mode == k_locality_mode::clustered) {
        require(
            position < k &&
            k <= count,
            "Invalid RC-V2-03B clustered selection");

        return position;
    }

    return k_source_index(
        count,
        k,
        position);
}

row k_modify_locality(
    graph_manager& manager,
    std::size_t count,
    std::size_t k,
    const k_baseline& baseline,
    k_locality_mode mode) {

    const auto contribution_before =
        access::contribution_storage(manager);
    const auto strings_before =
        access::string_storage(manager);
    const auto started =
        clock_type::now();

    auto transaction =
        manager.begin_build(
            graph_build_mode::incremental);

    clock_type::duration replace_elapsed{};
    clock_type::duration declare_elapsed{};

    for (std::size_t position = 0;
         position < k;
         ++position) {
        const auto index =
            k_locality_index(
                count,
                k,
                position,
                mode);

        graph_update::source_replacement replacement;

        auto phase_begin =
            clock_type::now();

        const auto replace_result =
            transaction.graph_state().replace_source(
                baseline.sources[index],
                replacement);

        replace_elapsed +=
            clock_type::now() - phase_begin;

        require(
            replace_result.ok(),
            "RC-V2-03B Source replacement failed");

        const enum_build_data data{
            enum_definition_state::opaque,
            false,
            builtin_type::integer,
            {}
        };

        type_id entity;
        type_handle type;

        phase_begin =
            clock_type::now();

        const auto declare_result =
            replacement.add_named_enum(
                baseline.names[index],
                data,
                entity,
                type);

        declare_elapsed +=
            clock_type::now() - phase_begin;

        require(
            declare_result.ok(),
            "RC-V2-03B enum materialization failed");
    }

    const auto setup_done =
        clock_type::now();

    auto result =
        finish_row(
            manager,
            transaction,
            count,
            mode == k_locality_mode::clustered
                ? "k_modify_clustered"
                : "k_modify_spread",
            started,
            setup_done,
            contribution_before,
            strings_before);

    result.setup_replace_source_ms =
        milliseconds(replace_elapsed);
    result.setup_declare_named_ms =
        milliseconds(declare_elapsed);

    return result;
}

void run_locality_case(
    std::size_t count,
    std::size_t k,
    k_locality_mode mode) {

    graph_manager manager;

    require(
        manager.initialize().ok(),
        "RC-V2-03B manager initialization failed");

    const auto baseline =
        build_k_baseline(
            manager,
            count);

    const auto value =
        k_modify_locality(
            manager,
            count,
            k,
            baseline,
            mode);

    require_k_noop_gates(
        value,
        k);

    print_row(value);
}

void run_locality_matrix() {
    for (const auto count :
         std::array<std::size_t, 2>{
             8192,
             32768}) {
        for (const auto k :
             std::array<std::size_t, 2>{
                 64,
                 256}) {
            run_locality_case(
                count,
                k,
                k_locality_mode::clustered);

            run_locality_case(
                count,
                k,
                k_locality_mode::spread);
        }
    }
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

    require_k_noop_gates(modify, 1);

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

    require(
        add.strings_before.lookup_bucket_count ==
            add.strings_after.lookup_bucket_count,
        "G3 String Registry rehashed after bulk-reserved G0");

    require(
        add.strings_before.records_data ==
            add.strings_after.records_data,
        "G3 String Registry records relocated after bulk-reserved G0");

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

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string_view{argv[1]} == "--rc-v2-07") {
        print_header();
        run_typeref_resolution_matrix();

        std::cout
            << "RC-V2-07 TYPEREF RESOLUTION SCALING PASS\n";
        return 0;
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--rc-v2-06") {
        print_header();
        run_edge_scaling_matrix();

        std::cout
            << "RC-V2-06 DEPENDENCY EDGE SCALING PASS\n";
        return 0;
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--rc-v2-05") {
        print_header();
        run_multi_root_dependency_matrix();

        std::cout
            << "RC-V2-05 MULTI-ROOT DEPENDENCY DEDUP PASS\n";
        return 0;
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--rc-v2-04") {
        print_header();
        run_dependency_scaling_matrix();

        std::cout
            << "RC-V2-04 DEPENDENCY CLOSURE SCALING PASS\n";
        return 0;
    }

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

    for (const auto count :
         std::array<std::size_t, 2>{
             8192,
             32768}) {
        for (const auto k :
             std::array<std::size_t, 5>{
                 1,
                 4,
                 16,
                 64,
                 256}) {
            run_k_matrix(
                count,
                k);
        }
    }

    run_locality_matrix();
    run_dependency_scaling_matrix();
    run_multi_root_dependency_matrix();
    run_edge_scaling_matrix();
    run_typeref_resolution_matrix();

    std::cout
        << "RC-V2-07 TYPEREF RESOLUTION SCALING PASS\n";
    return 0;
}
