#include "project_context.hpp"

#include "../diagnostics/diagnostic_descriptor.hpp"
#include "../metrics/scoped_timer.hpp"

namespace cw::server {

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
    log.info(
        log_component::project,
        operation,
        "project initialization started");

    auto result = graphs.initialize();

    if (!result.ok()) {
        diagnostic_records.emit({
            diagnostics::source_initialization_failed.id,
            diagnostics::source_initialization_failed.default_severity,
            operation,
            {},
            "while initializing the project"
        });

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
        diagnostic_records.emit({
            diagnostics::shm_initialization_failed.id,
            diagnostics::shm_initialization_failed.default_severity,
            operation,
            {},
            "while initializing the project"
        });

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

    return result;
}

// Restores canonical Graph state first, then attaches Runtime only after the
// compiled Graph has been loaded successfully.
status project_context::load_compiled_checkpoint(
    const std::filesystem::path& path,
    metrics_store& metrics) noexcept {

    auto result = graphs.load_compiled_checkpoint(path, &metrics);

    if (!result.ok()) {
        return result;
    }

    result = runtime_instance.attach(graphs.compiled_graph());

    if (!result.ok()) {
        return result;
    }

    return {};
}

void project_context::shutdown() noexcept {
}

const diagnostic_buffer& project_context::diagnostics() const noexcept {
    return diagnostic_records;
}

project_state project_context::state() const noexcept {
    return graphs.state();
}

status project_context::runtime_access(const runtime*& output) const noexcept {
    output = nullptr;

    const graph* runnable = nullptr;
    const auto result = graphs.runnable_graph(runnable);

    if (!result.ok()) {
        return result;
    }

    output = &runtime_instance;
    return {};
}

// Source checkpoints restore Source Manager persistence only; they do not
// publish canonical G or make Runtime runnable.
status project_context::load_source_checkpoint(
    const std::filesystem::path& path) noexcept {

    return graphs.load_source_checkpoint(path);
}

} // namespace cw::server
