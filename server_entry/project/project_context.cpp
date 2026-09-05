#include "project_context.hpp"

#include "builder/project_builder.hpp"
#include "frontend/source_frontend_generation.hpp"
#include "project_composition_resolver.hpp"
#include "project_configuration_loader.hpp"
#include "../diagnostics/diagnostic_descriptor.hpp"
#include "../metrics/scoped_timer.hpp"
#include "../metrics/source_acquisition_telemetry.hpp"

#include <string>
#include <string_view>

namespace cw::server {
namespace {

bool try_emit(
    diagnostic_buffer& diagnostics,
    const diagnostic_descriptor& descriptor,
    operation_id operation,
    std::string_view detail = {}) noexcept {

    try {
        diagnostics.emit({
            descriptor.id,
            descriptor.default_severity,
            operation,
            {},
            std::string{detail}
        });
        return true;
    }
    catch (...) {
        return false;
    }
}

} // namespace

status project_context::initialize(
    operation_id operation,
    logger& log,
    metrics_store& metrics) noexcept {

    metrics.increment(metric_id::project_initializations);
    scoped_timer project_timer{
        metrics,
        metric_id::project_initialization_duration
    };

    diagnostic_records.clear();
    runtime_attached.store(false, std::memory_order_release);

    log.info(
        log_component::project,
        operation,
        "project initialization started");

    auto result = graphs.initialize();

    if (!result.ok()) {
        try_emit(
            diagnostic_records,
            diagnostics::source_initialization_failed,
            operation,
            "while initializing the project");

        log.error(
            log_component::source,
            operation,
            "source manager initialization failed");

        log.error(
            log_component::project,
            operation,
            "project initialization failed");

        return result;
    }

    metrics.increment(metric_id::shm_initializations);

    {
        scoped_timer shm_timer{
            metrics,
            metric_id::shm_initialization_duration
        };

        result = shared_memory_region.initialize();
    }

    if (!result.ok()) {
        try_emit(
            diagnostic_records,
            diagnostics::shm_initialization_failed,
            operation,
            "while initializing the project");

        log.error(
            log_component::shm,
            operation,
            "shared memory initialization failed");

        log.error(
            log_component::project,
            operation,
            "project initialization failed");

        return result;
    }

    log.info(
        log_component::project,
        operation,
        "project initialization completed");

    return {};
}

status project_context::load_project(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    logger& log,
    metrics_store& metrics) noexcept {

    diagnostic_records.clear();
    runtime_attached.store(false, std::memory_order_release);

    log.info(
        log_component::project,
        operation,
        "project load started");

    project_configuration configuration;

    auto result = load_project_configuration_file(
        configuration_path,
        operation,
        diagnostic_records,
        metrics,
        configuration);

    if (!result.ok()) {
        log.error(
            log_component::project,
            operation,
            "project configuration load failed");
        return result;
    }

    result = graphs.initialize(configuration.abi);

    if (!result.ok()) {
        try_emit(
            diagnostic_records,
            diagnostics::source_initialization_failed,
            operation,
            "while applying project ABI");

        log.error(
            log_component::source,
            operation,
            "source manager initialization failed");
        return result;
    }

    auto transaction =
        graphs.begin_build(graph_build_mode::rebuild);

    result = resolve_project_composition(
        configuration_path,
        configuration,
        operation,
        diagnostic_records,
        metrics,
        transaction.sources());

    if (!result.ok()) {
        log.error(
            log_component::project,
            operation,
            "project composition failed");
        return result;
    }

    project_builder builder;
    source_acquisition_telemetry acquisition{metrics.mode()};
    source_frontend_generation frontend{transaction};

    const auto rebuilt = frontend.rebuild(
        operation,
        diagnostic_records,
        acquisition,
        builder);

    acquisition.flush_to(metrics);

    if (!rebuilt.ok()) {
        log.error(
            log_component::source,
            operation,
            "project source build failed");
        return rebuilt.semantic;
    }

    result = runtime_instance.attach(
        graphs.compiled_graph());

    if (!result.ok()) {
        try_emit(
            diagnostic_records,
            diagnostics::runtime_attach_failed,
            operation);

        log.error(
            log_component::runtime,
            operation,
            "runtime attachment failed");
        return result;
    }

    runtime_attached.store(true, std::memory_order_release);

    log.info(
        log_component::project,
        operation,
        "project load completed");

    return {};
}

status project_context::load_compiled_checkpoint(
    const std::filesystem::path& path,
    metrics_store& metrics) noexcept {

    runtime_attached.store(false, std::memory_order_release);

    auto result = graphs.load_compiled_checkpoint(path, &metrics);

    if (!result.ok()) {
        return result;
    }

    result = runtime_instance.attach(graphs.compiled_graph());

    if (!result.ok()) {
        return result;
    }

    runtime_attached.store(true, std::memory_order_release);
    return {};
}

void project_context::shutdown() noexcept {
    runtime_attached.store(false, std::memory_order_release);
}

const diagnostic_buffer& project_context::diagnostics() const noexcept {
    return diagnostic_records;
}

project_state project_context::state() const noexcept {
    const auto graph_state = graphs.state();

    if (graph_state != project_state::valid) {
        return graph_state;
    }

    return runtime_attached.load(std::memory_order_acquire)
        ? project_state::valid
        : project_state::error;
}

status project_context::runtime_access(const runtime*& output) const noexcept {
    output = nullptr;

    if (!runtime_attached.load(std::memory_order_acquire)) {
        return {status_code::invalid_state};
    }

    const graph* runnable = nullptr;
    const auto result = graphs.runnable_graph(runnable);

    if (!result.ok()) {
        return result;
    }

    output = &runtime_instance;
    return {};
}

status project_context::load_source_checkpoint(
    const std::filesystem::path& path) noexcept {

    runtime_attached.store(false, std::memory_order_release);
    return graphs.load_source_checkpoint(path);
}

} // namespace cw::server
