// Identical fixture for baseline/candidate. Select implementation via include
// directories and compilation inputs, never through benchmark conditionals.
#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"

#include "project/parser/parser.hpp"
#include "project/frontend/source_publisher.hpp"
#include "project/builder/project_builder.hpp"
#include "metrics/source_acquisition_telemetry.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

using namespace cw::server;
using Clock = std::chrono::steady_clock;

static void require(bool value) { if (!value) throw std::runtime_error("fixture invariant failed"); }
static double ms(Clock::time_point begin) { return std::chrono::duration<double, std::milli>(Clock::now() - begin).count(); }
struct Fixture {
    graph_manager manager;
    std::filesystem::path root;
    Fixture() {
        root = std::filesystem::temp_directory_path() / ("entry-repair-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(Clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        std::ofstream(root / "a.cpp") << "// fixture\n";
        std::ofstream(root / "b.cpp") << "// fixture\n";
        require(manager.initialize().ok());
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    source_id source(graph_build_transaction& tx, const char* name) {
        source_id id;
        source_acquisition_telemetry telemetry{metrics_mode::off};
        require(tx.sources().resolve(root / name, project_item_role::source, id).ok());
        require(tx.sources().acquire(id, telemetry).ok());
        return id;
    }
};
static string_id bind_name(graph_build_transaction& tx, const std::string& name) {
    string_id id; require(tx.strings().bind(name, id).ok()); return id;
}
static type_handle aggregate(graph_update::source_replacement& r, string_id name, std::span<const member_build> members, bool defined = true) {
    stable_id id; type_handle handle;
    require(r.add_named_type(name, defined ? aggregate_definition_state::defined : aggregate_definition_state::declared, id, handle).ok());
    if (defined) require(r.define_members(handle, members, {}).ok());
    return handle;
}
static void row(const char* shape, std::size_t n, int run, const char* phase, double total, const graph_manager& m) {
    const auto s = m.compiled_graph().storage_snapshot_for_testing();
    PROCESS_MEMORY_COUNTERS_EX p{}; p.cb = sizeof(p);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p), sizeof(p)) != 0);
    std::cout << shape << ',' << n << ',' << run << ',' << phase << ',' << total << ','
        << s.identity_size << ',' << s.identity_capacity << ',' << s.types_size << ','
        << s.member_records_size << ',' << s.member_records_capacity << ',' << s.canonical_types_size << ','
        << p.PrivateUsage << ',' << p.WorkingSetSize << ',' << p.PeakWorkingSetSize << '\n';
}
static void parse_case(const char* shape, std::size_t n) {
    std::string text = std::string(shape) == "enum" ? "enum Large : int {" : "struct Large {";
    for (std::size_t i = 0; i != n; ++i)
        text += std::string(shape) == "enum" ? "E" + std::to_string(i) + "=" + std::to_string(i) + "," : "int m" + std::to_string(i) + ";";
    text += "};";
    for (int run = 0; run != 4; ++run) {
        Fixture f;
        auto tx = f.manager.begin_build(graph_build_mode::rebuild);
        const auto source = f.source(tx, "a.cpp");
        source_context context;
        source_environment environment;
        const auto parse_begin = Clock::now();
        require(parse_source({source, text}, environment, operation_id{1}, context).ok());
        const auto parse_ms = ms(parse_begin);
        source_build_entry facts;
        require(context.release_facts(source, facts).ok());
        context.reset();
        project_builder builder; diagnostic_buffer diagnostics;
        const auto publish_begin = Clock::now();
        require(publish_source_entry(tx, facts, builder, operation_id{1}, diagnostics).ok());
        require(tx.commit().ok());
        const auto publish_ms = ms(publish_begin);
        row(shape, n, run, "parse", parse_ms, f.manager);
        row(shape, n, run, "publish_commit", publish_ms, f.manager);
        require(f.manager.compiled_graph().entity_count() == 1);
    }
}
static void edges(const char* shape, std::size_t n) {
    Fixture f;
    source_id changed;
    std::vector<string_id> bases, holders;
    string_id member;
    {
        auto tx = f.manager.begin_build(graph_build_mode::rebuild);
        const auto source = f.source(tx, "a.cpp"); changed = f.source(tx, "b.cpp");
        member = bind_name(tx, "m");
        graph_update::source_replacement r;
        require(tx.graph_state().replace_source(source, r).ok());
        const auto base_count = std::string(shape) == "fan_out" ? n : 2;
        for (std::size_t i = 0; i != base_count; ++i) {
            bases.push_back(bind_name(tx, "Base" + std::to_string(i)));
            aggregate(r, bases.back(), {}, false);
        }
        require(tx.graph_state().replace_source(changed, r).ok());
        const auto holder_count = std::string(shape) == "fan_out" ? 1 : n;
        for (std::size_t i = 0; i != holder_count; ++i) {
            holders.push_back(bind_name(tx, "Holder" + std::to_string(i)));
            std::vector<member_build> members;
            for (std::size_t j = 0; j != (holder_count == 1 ? n : 1); ++j)
                members.push_back({bind_name(tx, "m" + std::to_string(j)), std::nullopt, bases[holder_count == 1 ? j : 0], 0, 0});
            aggregate(r, holders.back(), members);
        }
        require(tx.commit().ok());
    }
    for (int run = 0; run != 4; ++run) {
        auto tx = f.manager.begin_build(graph_build_mode::incremental);
        graph_update::source_replacement r;
        require(tx.graph_state().replace_source(changed, r).ok());
        std::vector<member_build> members;
        for (std::size_t i = 0; i != holders.size(); ++i) {
            members.clear();
            const bool fan_out = std::string(shape) == "fan_out";
            for (std::size_t j = 0; j != (fan_out ? n : 1); ++j)
                members.push_back({bind_name(tx, "m" + std::to_string(j)), std::nullopt,
                    bases[fan_out ? j : static_cast<std::size_t>((run + 1) % 2)], 0, 0});
            aggregate(r, holders[i], members);
        }
        const auto begin = Clock::now();
        require(tx.commit().ok());
        row(shape, n, run, "commit", ms(begin), f.manager);
        require(f.manager.compiled_graph().entity_count() == holders.size() + bases.size());
    }
}
static void churn() {
    Fixture f;
    source_id source;
    stable_id stable;
    for (int run = 0; run <= 503; ++run) {
        const bool rebuild = run == 0 || run > 500;
        const auto begin = Clock::now();
        {
            auto tx = f.manager.begin_build(rebuild ? graph_build_mode::rebuild : graph_build_mode::incremental);
            if (rebuild) source = f.source(tx, "a.cpp");
            const auto name = bind_name(tx, "Churn");
            if (rebuild) require(tx.graph_state().reserve_rebuild(1, 130, 1, 1).ok());
            std::vector<member_build> members;
            for (int i = 0; i != 128; ++i) members.push_back({bind_name(tx, "m" + std::to_string(i)), builtin_type::integer, {}, 0, 0});
            graph_update::source_replacement r;
            require(tx.graph_state().replace_source(source, r).ok());
            aggregate(r, name, members);
            require(tx.commit().ok());
            const auto id = f.manager.compiled_graph().find_id(name);
            if (run == 0) stable = id;
            require(id == stable);
        }
        if (run == 0 || run == 100 || run >= 500) row("churn", 128, run, rebuild ? "g0" : "delta", ms(begin), f.manager);
    }
}
static void type_churn() {
    Fixture f;
    source_id source;
    stable_id identities[8]{};
    for (int run = 0; run <= 503; ++run) {
        const bool rebuild = run == 0 || run > 500;
        const auto begin = Clock::now();
        {
            auto tx = f.manager.begin_build(rebuild ? graph_build_mode::rebuild : graph_build_mode::incremental);
            if (rebuild) source = f.source(tx, "a.cpp");
            const auto key = run > 500 ? 500 % 8 : run % 8;
            const auto name = bind_name(tx, "Rotating" + std::to_string(key));
            if (rebuild) require(tx.graph_state().reserve_rebuild(1, 1, 1, 1).ok());
            graph_update::source_replacement r;
            require(tx.graph_state().replace_source(source, r).ok());
            aggregate(r, name, {});
            require(tx.commit().ok());
            const auto id = f.manager.compiled_graph().find_id(name);
            if (!identities[key]) identities[key] = id;
            require(identities[key] == id && f.manager.compiled_graph().entity_count() == 1);
        }
        if (run == 0 || run == 100 || run >= 500)
            row("type_churn", 8, run, rebuild ? "g0" : "delta", ms(begin), f.manager);
    }
}
int main() {
    try {
        std::cout << std::fixed << std::setprecision(6)
            << "shape,n,run,phase,ms,identity_size,identity_capacity,type_slots,member_size,member_capacity,typeref_size,private_bytes,working_set,peak_working_set\n";
        for (auto n : {1024u, 4096u, 16384u}) { parse_case("aggregate", n); parse_case("enum", n); }
        for (auto n : {512u, 2048u, 8192u, 16384u}) { edges("fan_out", n); edges("fan_in", n); }
        churn();
        type_churn();
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

