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

using namespace cw::server;

static void write_file(const std::filesystem::path& path, const char* text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    assert(file);
    file << text;
    file.close();
}

static status build(graph_manager& manager,
                    const std::filesystem::path& source,
                    const std::filesystem::path& compiled = {}) {
    auto tx = manager.begin_build();
    if (tx.sources().roots().empty()) {
        auto r = tx.sources().add(source, project_item_role::source);
        if (!r.ok()) return r;
    }
    source_frontend_generation frontend{tx};
    diagnostic_buffer diagnostics;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    project_builder builder;
    const auto result = frontend.rebuild(
        operation_id{}, diagnostics, telemetry, builder,
        {}, nullptr, compiled);
    if (!result.ok()) {
        for (const auto& d : diagnostics.records())
            std::cerr << "diag " << d.id.value() << " src=" << d.location.source.value() << '\n';
    }
    return result.semantic;
}

static const entity_entry* find(const graph_manager& manager, const char* name) {
    auto id = manager.strings().find(name);
    return id ? manager.compiled_graph().find(id) : nullptr;
}

int main() {
    const auto dir = std::filesystem::path{"/mnt/data/server_entry_repair/testrepo/reconstruct_case"};
    std::filesystem::create_directories(dir);
    const auto source = dir / "main.cpp";
    const auto artifact = dir / "project.cwc";
    std::error_code ec;
    std::filesystem::remove(artifact, ec);

    write_file(source, "struct A; struct B;\n");

    graph_manager initial;
    abi_configuration abi{};
    abi.target = abi_target::posix_x64;
    abi.pack = 8;
    assert(initial.initialize(abi).ok());
    assert(build(initial, source, artifact).ok());

    const auto* initial_a = find(initial, "A");
    const auto* initial_b = find(initial, "B");
    assert(initial_a && initial_b);
    const auto a_id = initial.compiled_graph().find_id(initial.strings().find("A")).value();
    const auto b_id = initial.compiled_graph().find_id(initial.strings().find("B")).value();
    assert(a_id != b_id);

    graph_manager restored;
    assert(restored.initialize(abi).ok());
    assert(restored.load_compiled_checkpoint(artifact, nullptr).ok());
    assert(restored.state() == project_state::valid);
    assert(find(restored, "A") && find(restored, "B"));

    // First Source build after compiled-only load must reconstruct provenance.
    write_file(source, "struct A; struct C;\n");
    assert(build(restored, source).ok());

    const auto* rebuilt_a = find(restored, "A");
    const auto* rebuilt_b = find(restored, "B");
    const auto* rebuilt_c = find(restored, "C");
    assert(rebuilt_a && !rebuilt_b && rebuilt_c);
    assert(restored.compiled_graph().find_id(restored.strings().find("A")).value() == a_id);
    assert(restored.compiled_graph().find_id(restored.strings().find("C")).value() > b_id);

    // Once provenance has been reconstructed, ordinary incremental replacement
    // must continue to remove the previous Source contribution correctly.
    const auto c_id = restored.compiled_graph().find_id(restored.strings().find("C")).value();
    write_file(source, "struct A; struct D;\n");
    assert(build(restored, source).ok());

    const auto* final_a = find(restored, "A");
    const auto* final_c = find(restored, "C");
    const auto* final_d = find(restored, "D");
    assert(final_a && !final_c && final_d);
    assert(restored.compiled_graph().find_id(restored.strings().find("A")).value() == a_id);
    assert(restored.compiled_graph().find_id(restored.strings().find("D")).value() > c_id);

    std::cout << "PASS A=" << a_id
              << " B=" << b_id
              << " C=" << c_id
              << " D=" << restored.compiled_graph().find_id(restored.strings().find("D")).value() << '\n';
}
