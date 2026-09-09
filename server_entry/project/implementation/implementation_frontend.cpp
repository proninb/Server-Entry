#include "implementation_frontend.hpp"

#include "implementation_context.hpp"
#include "implementation_parser.hpp"
#include "../graph/graph.hpp"
#include "../source/source_manager.hpp"
#include "../string/string_registry.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace cw::server {
namespace {

status copy_diagnostics(
    const diagnostic_buffer& source,
    diagnostic_buffer& destination) noexcept {

    try {
        destination.reserve(
            destination.records().size() +
            source.records().size());

        for (const auto& record : source.records()) {
            destination.emit(record);
        }

        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status emit_acquisition_failure(
    diagnostic_buffer& diagnostics,
    operation_id operation,
    source_id source,
    status result) noexcept {

    try {
        diagnostics.emit({
            diagnostics::source_acquisition_failed.id,
            diagnostics::source_acquisition_failed.default_severity,
            operation,
            {source, 0, 0},
            {}
        });
    }
    catch (...) {
        return {status_code::initialization_failed};
    }

    return result;
}

template <typename GetView>
status parse_selected_sources(
    std::span<const source_id> selected_sources,
    GetView&& get_view,
    const graph_type_view& types,
    const string_registry& strings,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<implementation_facts_storage>& output) noexcept {

    output.clear();

    if (selected_sources.empty()) {
        return {};
    }

    try {
        struct parse_slot {
            source_id source{};
            implementation_facts_storage facts;
            diagnostic_buffer diagnostics;
            status result{};
        };

        std::vector<parse_slot> slots(
            selected_sources.size());

        for (std::size_t index = 0;
             index < selected_sources.size();
             ++index) {
            if (!selected_sources[index]) {
                return {status_code::configuration_failed};
            }

            slots[index].source = selected_sources[index];
        }

        const auto hardware_threads =
            std::thread::hardware_concurrency();

        const auto worker_count =
            (std::min)(
                std::size_t{32},
                (std::min)(
                    slots.size(),
                    static_cast<std::size_t>(
                        hardware_threads == 0
                            ? 1
                            : hardware_threads)));

        std::atomic_size_t next_index{0};
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);

        for (std::size_t worker_index = 0;
             worker_index < worker_count;
             ++worker_index) {
            workers.emplace_back([&]() {
                implementation_context context;

                for (;;) {
                    const auto index =
                        next_index.fetch_add(
                            1,
                            std::memory_order_relaxed);

                    if (index >= slots.size()) {
                        break;
                    }

                    auto& slot = slots[index];
                    source_view view;

                    slot.result =
                        get_view(
                            slot.source,
                            view);

                    if (!slot.result.ok()) {
                        slot.result =
                            emit_acquisition_failure(
                                slot.diagnostics,
                                operation,
                                slot.source,
                                slot.result);
                        continue;
                    }

                    slot.result =
                        parse_implementation_source(
                            view,
                            types,
                            strings,
                            operation,
                            context);

                    const auto copied =
                        copy_diagnostics(
                            context.diagnostics,
                            slot.diagnostics);

                    if (!copied.ok()) {
                        slot.result = copied;
                        context.reset();
                        continue;
                    }

                    if (slot.result.ok()) {
                        slot.result =
                            context.release_facts(
                                slot.source,
                                slot.facts);
                    }
                    else {
                        context.reset();
                    }
                }
            });
        }

        workers.clear();

        status first_failure;

        for (const auto& slot : slots) {
            const auto copied =
                copy_diagnostics(
                    slot.diagnostics,
                    diagnostics);

            if (!copied.ok()) {
                return copied;
            }

            if (first_failure.ok() &&
                !slot.result.ok()) {
                first_failure = slot.result;
            }
        }

        diagnostics.sort_deterministic();

        if (!first_failure.ok()) {
            return first_failure;
        }

        output.reserve(slots.size());

        for (auto& slot : slots) {
            output.push_back(
                std::move(slot.facts));
        }

        return {};
    }
    catch (...) {
        output.clear();
        return {status_code::initialization_failed};
    }
}

} // namespace

status implementation_frontend::build(
    const source_manager& sources,
    const graph_type_view& types,
    const string_registry& strings,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<implementation_facts_storage>& output) const noexcept {

    output.clear();

    try {
        std::vector<source_id> implementation_sources;
        std::vector<std::uint8_t> seen(
            sources.source_count() + 1,
            0);

        implementation_sources.reserve(
            sources.root_count());

        for (const auto root : sources.roots()) {
            if (!root.source ||
                root.source.value() > sources.source_count()) {
                return {status_code::configuration_failed};
            }

            if (root.role == project_item_role::project) {
                return {status_code::configuration_failed};
            }

            if (root.role != project_item_role::source) {
                continue;
            }

            auto& present = seen[root.source.value()];

            if (present != 0) {
                continue;
            }

            present = 1;
            implementation_sources.push_back(root.source);
        }

        return parse_selected_sources(
            implementation_sources,
            [&](source_id source, source_view& view) noexcept {
                return sources.get_view(source, view);
            },
            types,
            strings,
            operation,
            diagnostics,
            output);
    }
    catch (...) {
        output.clear();
        return {status_code::initialization_failed};
    }
}

status implementation_frontend::rebuild(
    const source_manager_update& sources,
    std::span<const source_id> dirty_sources,
    const graph_type_view& types,
    const string_registry& strings,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<implementation_facts_storage>& output) const noexcept {

    return parse_selected_sources(
        dirty_sources,
        [&](source_id source, source_view& view) noexcept {
            return sources.get_view(source, view);
        },
        types,
        strings,
        operation,
        diagnostics,
        output);
}

} // namespace cw::server
