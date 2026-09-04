#include "project/frontend/source_frontend_generation.hpp"
#include "project/builder/project_builder.hpp"
#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include "diagnostics/diagnostic_buffer.hpp"
#include "metrics/source_acquisition_telemetry.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

using namespace cw::server;

static std::string name(int value) {
    std::ostringstream out;
    out << 'H' << std::setw(2) << std::setfill('0') << value;
    return out.str();
}

int main() {
    constexpr int count = 32;
    const auto directory = std::filesystem::path{
        "/mnt/data/server_entry_repair/testrepo/discovery_fanout"};
    std::filesystem::create_directories(directory);

    for (int index = 0; index < count; ++index) {
        std::ofstream file(directory / (name(index) + ".hpp"),
                           std::ios::binary | std::ios::trunc);
        assert(file);
        file << "struct " << name(index) << ";\n";
        // Make every acquisition perform a real content read/hash on first build.
        for (int line = 0; line < 256; ++line)
            file << "// padding " << index << ' ' << line << " abcdefghijklmnopqrstuvwxyz\n";
    }

    const auto root = directory / "root.cpp";
    {
        std::ofstream file(root, std::ios::binary | std::ios::trunc);
        assert(file);
        for (int index = 0; index < count; ++index)
            file << "#include \"" << name(index) << ".hpp\"\n";
        file << "struct Root { ";
        for (int index = 0; index < count; ++index)
            file << name(index) << "* h" << index << "; ";
        file << "};\n";
    }

    graph_manager manager;
    abi_configuration abi{};
    abi.target = abi_target::posix_x64;
    abi.pack = 8;
    assert(manager.initialize(abi).ok());

    auto transaction = manager.begin_build();
    assert(transaction.sources().add(root, project_item_role::source).ok());

    source_frontend_generation frontend{transaction};
    diagnostic_buffer diagnostics;
    source_acquisition_telemetry telemetry{metrics_mode::detailed};
    project_builder builder;

    const auto result = frontend.rebuild(
        operation_id{}, diagnostics, telemetry, builder);

    if (!result.ok()) {
        for (const auto& record : diagnostics.records())
            std::cerr << "diag=" << record.id.value()
                      << " source=" << record.location.source.value()
                      << " offset=" << record.location.offset << '\n';
    }

    assert(result.ok());
    assert(manager.state() == project_state::valid);
    assert(manager.compiled_graph().entity_count() == count + 1);
    assert(manager.strings().find("Root"));

    for (int index = 0; index < count; ++index)
        assert(manager.strings().find(name(index)));

    std::cout << "PASS fanout=" << count << '\n';
}
