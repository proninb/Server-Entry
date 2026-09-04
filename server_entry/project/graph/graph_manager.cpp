#include "graph_manager.hpp"

#include "compiled_persistence.hpp"
#include "graph_build_transaction.hpp"
#include "../../metrics/metrics_store.hpp"

#include <chrono>

namespace cw::server {

status graph_manager::initialize(abi_configuration abi) noexcept {
    auto result = source_manager_state.initialize();

    if (!result.ok()) {
        return result;
    }

    result = string_registry_state.initialize();

    if (!result.ok()) {
        return result;
    }

    result = graph_state.initialize(abi);

    // Initialization does not publish a runnable project. Runtime becomes
    // eligible only after a committed build or compiled-checkpoint load.
    current_state.store(project_state::error, std::memory_order_release);

    return result;
}

graph_build_transaction graph_manager::begin_build() noexcept {
    current_state.store(project_state::building, std::memory_order_release);
    return graph_build_transaction{*this};
}

void graph_manager::complete_build(bool success) noexcept {
    current_state.store(
        success ? project_state::valid : project_state::error,
        std::memory_order_release);
}

status graph_manager::runnable_graph(const graph*& output) const noexcept {
    output = nullptr;

    if (state() != project_state::valid) {
        return {status_code::invalid_state};
    }

    output = &graph_state;
    return {};
}

const graph& graph_manager::compiled_graph() const noexcept {
    return graph_state;
}

const string_registry& graph_manager::strings() const noexcept {
    return string_registry_state;
}

status graph_manager::save_source_checkpoint(const std::filesystem::path& path) const noexcept {
    return source_manager_state.save_checkpoint(path);
}

status graph_manager::save_source_checkpoint(
    const std::filesystem::path& path,
    metrics_store* metrics) const noexcept {

    return source_manager_state.save_checkpoint(path, metrics);
}

status graph_manager::load_source_checkpoint(const std::filesystem::path& path) noexcept {
    current_state.store(project_state::error, std::memory_order_release);
    return source_manager_state.load_checkpoint(path);
}

status graph_manager::save_compiled_checkpoint(
    const std::filesystem::path& path,
    metrics_store* metrics) const noexcept {

    return write_compiled_checkpoint(path, string_registry_state, graph_state, metrics);
}

status graph_manager::load_compiled_checkpoint(
    const std::filesystem::path& path,
    metrics_store* metrics) noexcept {

    current_state.store(project_state::building, std::memory_order_release);

    try {
        string_registry imported_strings;
        graph imported_graph;

        auto result = imported_strings.initialize();

        if (result.ok()) {
            result = imported_graph.initialize();
        }

        if (result.ok()) {
            result = read_compiled_checkpoint(path, imported_strings, imported_graph, metrics);
        }

        if (!result.ok()) {
            current_state.store(project_state::error, std::memory_order_release);
            return result;
        }

        const auto publish_begin = std::chrono::steady_clock::now();

        string_registry_state.swap_compiled(imported_strings);
        graph_state.swap_compiled(imported_graph);

        current_state.store(project_state::valid, std::memory_order_release);

        if (metrics) {
            metrics->record_duration(
                metric_id::compiled_load_publish_duration,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - publish_begin));
        }

        return {};
    }
    catch (...) {
        current_state.store(project_state::error, std::memory_order_release);
        return {status_code::initialization_failed};
    }
}

} // namespace cw::server
