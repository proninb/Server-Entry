#include "project/frontend/source_frontend_generation.hpp"
#include "project/builder/project_builder.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include "project/graph/graph_manager.hpp"
#include "project/parser/parser.hpp"
#include "diagnostics/diagnostic_buffer.hpp"
#include "metrics/source_acquisition_telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>

using namespace cw::server;

class tracking_backend final : public parser_backend {
public:
    status parse(
        source_view source,
        std::span<const parser_token> tokens,
        const source_environment& environment,
        const language_configuration&,
        operation_id operation,
        source_context& context) const noexcept override {

        const auto active_now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
        auto previous = maximum.load(std::memory_order_relaxed);
        while (previous < active_now &&
               !maximum.compare_exchange_weak(previous, active_now,
                                              std::memory_order_relaxed)) {}

        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        const auto result = parse_source_tokens(
            source, tokens, environment, operation, context);
        active.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }

    mutable std::atomic<unsigned> active{0};
    mutable std::atomic<unsigned> maximum{0};
};

static std::string type_name(int index) {
    std::ostringstream out;
    out << 'T' << std::setw(3) << std::setfill('0') << index;
    return out.str();
}

static std::vector<std::uint32_t> build_project(
    const std::filesystem::path& directory,
    const std::vector<int>& order,
    tracking_backend& backend) {

    graph_manager manager;
    abi_configuration abi{};
    abi.target = abi_target::posix_x64;
    abi.pack = 8;
    assert(manager.initialize(abi).ok());

    auto transaction = manager.begin_build();
    std::vector<source_id> sources;
    sources.reserve(order.size());

    for (const auto index : order) {
        const auto path = directory / (type_name(index) + ".cpp");
        assert(transaction.sources().add(path, project_item_role::source).ok());
        sources.push_back(transaction.sources().roots().back().source);
    }

    source_frontend_generation frontend{transaction, backend};
    diagnostic_buffer diagnostics;
    source_acquisition_telemetry telemetry{metrics_mode::off};
    project_builder builder;

    const auto result = frontend.rebuild(
        operation_id{}, diagnostics, telemetry, builder);

    if (!result.ok()) {
        for (const auto& record : diagnostics.records()) {
            std::cerr << "diagnostic id=" << record.id.value()
                      << " source=" << record.location.source.value()
                      << " offset=" << record.location.offset << '\n';
        }
    }

    assert(result.ok());
    assert(manager.state() == project_state::valid);
    assert(manager.compiled_graph().entity_count() == order.size());

    std::vector<std::uint32_t> ids;
    ids.reserve(order.size());

    for (int index = 0; index < static_cast<int>(order.size()); ++index) {
        const auto name = type_name(index);
        const auto string = manager.strings().find(name);
        assert(string);
        const auto* entity = manager.compiled_graph().find(string);
        assert(entity);
        ids.push_back(manager.compiled_graph().find_id(string).value());
    }

    return ids;
}

int main() {
    constexpr int count = 48;
    const auto directory = std::filesystem::path{
        "/mnt/data/server_entry_repair/testrepo/parallel_sources"};
    std::filesystem::create_directories(directory);

    for (int index = 0; index < count; ++index) {
        std::ofstream file(directory / (type_name(index) + ".cpp"),
                           std::ios::binary | std::ios::trunc);
        file << "struct " << type_name(index) << ";\n";
    }

    std::vector<int> forward(count);
    std::iota(forward.begin(), forward.end(), 0);
    auto reverse = forward;
    std::reverse(reverse.begin(), reverse.end());

    tracking_backend first_backend;
    const auto first = build_project(directory, forward, first_backend);

    tracking_backend second_backend;
    const auto second = build_project(directory, reverse, second_backend);

    assert(first == second);
    for (std::size_t index = 0; index < first.size(); ++index) {
        assert(first[index] == index + 1);
    }

    if (std::thread::hardware_concurrency() > 1) {
        assert(first_backend.maximum.load() > 1);
        assert(second_backend.maximum.load() > 1);
    }

    std::cout << "PASS max_parallel="
              << first_backend.maximum.load() << '\n';
}
