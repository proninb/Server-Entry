#define main original_graph_manager_tests_main
#include "../../tests/graph_manager_tests.cpp"
#undef main
#include <chrono>
#include <stdexcept>
#include <vector>
#include "../../server_entry/project/graph/compiled_state.hpp"

static void require_audit(bool condition) {
    if (!condition) throw std::runtime_error("audit fixture setup failed");
}

static void move_probe() {
    graph_manager manager;
    require_audit(manager.initialize().ok());
    auto original = manager.begin_build(graph_build_mode::rebuild);
    source_id source;
    require_audit(resolve_source(original, source_a, source));
    auto moved = std::move(original);
    graph_update::source_replacement replacement;
    const auto result = moved.graph_state().replace_source(source, replacement);
    std::cout << "move_replace_ok=" << result.ok() << '\n';
}

static void reuse_probe() {
    graph_manager manager;
    require_audit(manager.initialize().ok());
    source_id source;
    type_handle old_handle;
    string_id old_name, new_name;
    std::size_t old_ref = 0;
    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        require_audit(resolve_source(tx, source_a, source));
        require_audit(tx.strings().bind("Old", old_name).ok());
        graph_update::source_replacement replacement;
        require_audit(open_source(tx, source, replacement));
        stable_id id;
        require_audit(replacement.add_named_type(old_name, aggregate_definition_state::declared, id, old_handle).ok());
        require_audit(tx.commit().ok());
        compiled_graph_state state;
        require_audit(manager.compiled_graph().export_compiled(state).ok());
        for (std::size_t i = 0; i < state.canonical_types.size(); ++i)
            if (state.canonical_types[i].kind == static_cast<std::uint8_t>(canonical_type_kind::named) &&
                state.canonical_types[i].argument == old_handle.value()) old_ref = i;
    }
    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        graph_update::source_replacement replacement;
        require_audit(open_source(tx, source, replacement));
        require_audit(tx.commit().ok());
    }
    type_handle new_handle;
    std::size_t new_ref = 0;
    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        require_audit(tx.strings().bind("New", new_name).ok());
        graph_update::source_replacement replacement;
        require_audit(open_source(tx, source, replacement));
        stable_id id;
        require_audit(replacement.add_named_type(new_name, aggregate_definition_state::declared, id, new_handle).ok());
        require_audit(tx.commit().ok());
        compiled_graph_state state;
        require_audit(manager.compiled_graph().export_compiled(state).ok());
        for (std::size_t i = 0; i < state.canonical_types.size(); ++i)
            if (state.canonical_types[i].kind == static_cast<std::uint8_t>(canonical_type_kind::named) &&
                state.canonical_types[i].argument == new_handle.value()) new_ref = i;
    }
    std::cout << "reuse old_handle=" << old_handle.value() << " new_handle=" << new_handle.value()
              << " old_ref=" << old_ref << " new_ref=" << new_ref << '\n';
}

static void dependency_probe(std::size_t count) {
    graph_manager manager;
    require_audit(manager.initialize().ok());
    source_id aggregate_source;
    string_id aggregate_name;
    std::vector<member_build> members;
    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        source_id dependency_source;
        require_audit(resolve_source(tx, source_a, aggregate_source));
        require_audit(resolve_source(tx, source_b, dependency_source));
        require_audit(tx.strings().bind("Aggregate", aggregate_name).ok());
        graph_update::source_replacement deps;
        require_audit(open_source(tx, dependency_source, deps));
        members.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            string_id type_name, member_name;
            require_audit(tx.strings().bind("D" + std::to_string(i), type_name).ok());
            require_audit(tx.strings().bind("m" + std::to_string(i), member_name).ok());
            stable_id id; type_handle handle;
            require_audit(deps.add_named_type(type_name, aggregate_definition_state::declared, id, handle).ok());
            members.push_back({member_name, std::nullopt, type_name, 0, 0});
        }
        graph_update::source_replacement aggregate;
        require_audit(open_source(tx, aggregate_source, aggregate));
        stable_id id; type_handle handle;
        require_audit(aggregate.add_named_type(aggregate_name, aggregate_definition_state::defined, id, handle).ok());
        require_audit(aggregate.define_members(handle, members, {}).ok());
        require_audit(tx.commit().ok());
    }
    for (int run = 0; run < 4; ++run) {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        graph_update::source_replacement aggregate;
        require_audit(open_source(tx, aggregate_source, aggregate));
        stable_id id; type_handle handle;
        require_audit(aggregate.add_named_type(aggregate_name, aggregate_definition_state::defined, id, handle).ok());
        require_audit(aggregate.define_members(handle, members, {}).ok());
        const auto begin = std::chrono::steady_clock::now();
        require_audit(tx.commit().ok());
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::cout << "dependency," << count << ',' << run << ',' << ms << ','
                  << tx.timing().graph_final_prepare_ns / 1e6 << '\n';
    }
}

static void rebuild_growth_probe() {
    graph_manager manager;
    require_audit(manager.initialize().ok());
    for (int run = 0; run < 6; ++run) {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        source_id source; string_id name;
        require_audit(resolve_source(tx, source_a, source));
        require_audit(tx.strings().bind("Same", name).ok());
        require_audit(tx.graph_state().reserve_rebuild(1, 1, 1, 1).ok());
        graph_update::source_replacement replacement;
        require_audit(open_source(tx, source, replacement));
        stable_id id; type_handle handle;
        require_audit(replacement.add_named_type(name, aggregate_definition_state::declared, id, handle).ok());
        require_audit(tx.commit().ok());
        const auto snapshot = manager.compiled_graph().storage_snapshot_for_testing();
        std::cout << "rebuild_growth," << run << ",entities=" << manager.compiled_graph().entity_count()
                  << ",identity_size=" << snapshot.identity_size << ",identity_capacity=" << snapshot.identity_capacity << '\n';
    }
}

int main() {
    try {
        move_probe();
        reuse_probe();
        rebuild_growth_probe();
        for (auto count : {512u, 2048u, 8192u, 16384u}) dependency_probe(count);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
