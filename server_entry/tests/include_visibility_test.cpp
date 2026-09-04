#include "project/frontend/source_frontend_generation.hpp"
#include "project/builder/project_builder.hpp"
#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include "diagnostics/diagnostic_buffer.hpp"
#include "metrics/source_acquisition_telemetry.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

using namespace cw::server;

static void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    assert(f); f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

static source_rebuild_result run(const std::filesystem::path& root, graph_manager& manager, diagnostic_buffer& diagnostics) {
    abi_configuration abi{}; abi.target = abi_target::posix_x64; abi.pack = 8;
    assert(manager.initialize(abi).ok());
    auto tx = manager.begin_build();
    assert(tx.sources().add(root, project_item_role::source).ok());
    source_frontend_generation frontend{tx};
    source_acquisition_telemetry telemetry{metrics_mode::off};
    project_builder builder;
    return frontend.rebuild(operation_id{}, diagnostics, telemetry, builder);
}

int main() {
    const auto dir = std::filesystem::path{"/mnt/data/server_entry_repair/testrepo/include_cases"};
    std::filesystem::create_directories(dir);

    write_file(dir / "b.hpp", "struct B;\n");

    // Include is not retroactive: B is invisible before its textual include.
    write_file(dir / "before.cpp", "struct A { B b; };\n#include \"b.hpp\"\n");
    {
        graph_manager manager; diagnostic_buffer diagnostics;
        const auto result = run(dir / "before.cpp", manager, diagnostics);
        assert(!result.ok());
    }

    // The same name is visible after the include, including from a nested namespace
    // through normal lexical parent-scope lookup.
    write_file(dir / "after.cpp", "#include \"b.hpp\"\nnamespace N { struct A { B& b; }; }\n");
    {
        graph_manager manager; diagnostic_buffer diagnostics;
        const auto result = run(dir / "after.cpp", manager, diagnostics);
        assert(result.ok());
        const auto a_name = manager.strings().find("N::A");
        const auto b_name = manager.strings().find("B");
        assert(a_name && b_name);
        const auto* a = manager.compiled_graph().find(a_name);
        const auto* b = manager.compiled_graph().find(b_name);
        assert(a && b);
        const auto members = manager.compiled_graph().members(a->type);
        assert(members.size() == 1);
        const auto* ref = manager.compiled_graph().derived(members[0].type);
        assert(ref && ref->kind == derived_type_kind::lvalue_reference);
        type_handle named;
        assert(manager.compiled_graph().named(ref->child, named));
        assert(named == b->type);
    }

    // Transitive textual visibility: including b2.hpp imports c.hpp's interface.
    write_file(dir / "c.hpp", "struct C;\n");
    write_file(dir / "b2.hpp", "#include \"c.hpp\"\nstruct B2 { C c; };\n");
    write_file(dir / "transitive.cpp", "#include \"b2.hpp\"\nstruct A2 { B2 b; C c; };\n");
    {
        graph_manager manager; diagnostic_buffer diagnostics;
        const auto result = run(dir / "transitive.cpp", manager, diagnostics);
        assert(result.ok());
        assert(manager.compiled_graph().find(manager.strings().find("A2")));
        assert(manager.compiled_graph().find(manager.strings().find("B2")));
        assert(manager.compiled_graph().find(manager.strings().find("C")));
    }

    // Legal same-Entity redeclaration is coalesced in the exported interface.
    write_file(dir / "redecl.hpp", "struct X; struct X {};\n");
    write_file(dir / "redecl.cpp", "#include \"redecl.hpp\"\nstruct UsesX { X x; };\n");
    {
        graph_manager manager; diagnostic_buffer diagnostics;
        const auto result = run(dir / "redecl.cpp", manager, diagnostics);
        assert(result.ok());
        assert(manager.compiled_graph().find(manager.strings().find("X")));
        assert(manager.compiled_graph().find(manager.strings().find("UsesX")));
    }

    std::cout << "PASS\n";
}
