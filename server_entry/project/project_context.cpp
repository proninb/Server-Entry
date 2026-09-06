#include "project_context.hpp"

#include "builder/project_builder.hpp"
#include "frontend/source_frontend_generation.hpp"
#include "project_composition_resolver.hpp"
#include "project_configuration_loader.hpp"
#include "../diagnostics/diagnostic_descriptor.hpp"
#include "../metrics/scoped_timer.hpp"
#include "../metrics/source_acquisition_telemetry.hpp"

#include <chrono>
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

void emit_tracking_warning(
    diagnostic_buffer& diagnostics,
    operation_id operation,
    const diagnostic_descriptor& descriptor,
    logger& log,
    std::string_view message) noexcept {

    try_emit(
        diagnostics,
        descriptor,
        operation);

    log.write(
        log_level::warning,
        log_component::source,
        operation,
        message);
}

void record_duration_ns(
    metrics_store& metrics,
    metric_id id,
    std::uint64_t duration_ns) noexcept {

    metrics.record_duration(
        id,
        std::chrono::nanoseconds{
            static_cast<std::chrono::nanoseconds::rep>(
                duration_ns)
        });
}

void flush_g0_frontend_metrics(
    metrics_store& metrics,
    const source_frontend_summary& summary) noexcept {

    record_duration_ns(
        metrics,
        metric_id::frontend_g0_acquire_prepare_duration,
        summary.g0_acquire_prepare_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_acquire_execute_duration,
        summary.g0_acquire_execute_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_acquire_apply_duration,
        summary.g0_acquire_apply_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_discovery_duration,
        summary.g0_discovery_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_validation_duration,
        summary.g0_validation_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_semantic_duration,
        summary.g0_semantic_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_publish_duration,
        summary.g0_publish_ns);

    if (metrics.mode() == metrics_mode::detailed) {
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_reserve_scan_duration,
            summary.g0_publish_reserve_scan_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_string_reserve_duration,
            summary.g0_publish_string_reserve_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_source_replace_duration,
            summary.g0_publish_source_replace_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_name_duration,
            summary.g0_publish_enum_name_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_values_duration,
            summary.g0_publish_enum_values_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_sample_name_resolve_duration,
            summary.g0_publish_enum_sample_name_resolve_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_sample_name_intern_duration,
            summary.g0_publish_enum_sample_name_intern_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_sample_values_resolve_duration,
            summary.g0_publish_enum_sample_values_resolve_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_sample_values_intern_duration,
            summary.g0_publish_enum_sample_values_intern_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_duration,
            summary.g0_publish_enum_builder_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_value_copy_duration,
            summary.g0_publish_enum_builder_value_copy_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_mutation_duration,
            summary.g0_publish_enum_builder_graph_mutation_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_total_duration,
            summary.g0_publish_enum_builder_graph_sample_total_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_source_replacement_duration,
            summary.g0_publish_enum_builder_graph_sample_source_replacement_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_identity_duration,
            summary.g0_publish_enum_builder_graph_sample_identity_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_contribution_build_duration,
            summary.g0_publish_enum_builder_graph_sample_contribution_build_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_reconcile_duration,
            summary.g0_publish_enum_builder_graph_sample_reconcile_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_delta_duration,
            summary.g0_publish_enum_builder_graph_sample_delta_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_contribution_append_duration,
            summary.g0_publish_enum_builder_graph_sample_contribution_append_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_materialize_duration,
            summary.g0_publish_enum_builder_graph_sample_materialize_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_materialize_state_touch_duration,
            summary.g0_publish_enum_builder_graph_sample_materialize_state_touch_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_materialize_type_storage_duration,
            summary.g0_publish_enum_builder_graph_sample_materialize_type_storage_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_materialize_build_state_duration,
            summary.g0_publish_enum_builder_graph_sample_materialize_build_state_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_materialize_assign_type_duration,
            summary.g0_publish_enum_builder_graph_sample_materialize_assign_type_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_assign_type_handle_duration,
            summary.g0_publish_enum_builder_graph_sample_assign_type_handle_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_assign_type_touch_type_duration,
            summary.g0_publish_enum_builder_graph_sample_assign_type_touch_type_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_assign_type_candidate_store_duration,
            summary.g0_publish_enum_builder_graph_sample_assign_type_candidate_store_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_assign_type_named_type_ref_duration,
            summary.g0_publish_enum_builder_graph_sample_assign_type_named_type_ref_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_named_type_ref_existing_lookup_duration,
            summary.g0_publish_enum_builder_graph_sample_named_type_ref_existing_lookup_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_named_type_ref_canonical_append_duration,
            summary.g0_publish_enum_builder_graph_sample_named_type_ref_canonical_append_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_named_type_ref_mapping_append_duration,
            summary.g0_publish_enum_builder_graph_sample_named_type_ref_mapping_append_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_named_type_ref_index_emplace_duration,
            summary.g0_publish_enum_builder_graph_sample_named_type_ref_index_emplace_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_materialize_attach_duration,
            summary.g0_publish_enum_builder_graph_sample_materialize_attach_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_graph_sample_result_lookup_duration,
            summary.g0_publish_enum_builder_graph_sample_result_lookup_ns);

        if (summary.g0_publish_enum_builder_graph_call_count != 0) {
            metrics.increment(
                metric_id::frontend_g0_publish_enum_builder_graph_call_count,
                summary.g0_publish_enum_builder_graph_call_count);
        }

        if (summary.g0_publish_enum_builder_graph_sample_count != 0) {
            metrics.increment(
                metric_id::frontend_g0_publish_enum_builder_graph_sample_count,
                summary.g0_publish_enum_builder_graph_sample_count);
        }

        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_enum_builder_residual_duration,
            summary.g0_publish_enum_builder_residual_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_aggregate_builder_duration,
            summary.g0_publish_aggregate_builder_ns);
        record_duration_ns(
            metrics,
            metric_id::frontend_g0_publish_residual_duration,
            summary.g0_publish_residual_ns);

        if (summary.g0_publish_source_count != 0) {
            metrics.increment(
                metric_id::frontend_g0_publish_source_count,
                summary.g0_publish_source_count);
        }

        if (summary.g0_publish_enum_builder_count != 0) {
            metrics.increment(
                metric_id::frontend_g0_publish_enum_builder_count,
                summary.g0_publish_enum_builder_count);
        }

        if (summary.g0_publish_aggregate_builder_count != 0) {
            metrics.increment(
                metric_id::frontend_g0_publish_aggregate_builder_count,
                summary.g0_publish_aggregate_builder_count);
        }
    }

    record_duration_ns(
        metrics,
        metric_id::frontend_g0_commit_duration,
        summary.g0_commit_ns);

    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_source_prepare_duration,
        summary.g0_tx_source_prepare_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_string_prepare_duration,
        summary.g0_tx_string_prepare_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_graph_prepare_duration,
        summary.g0_tx_graph_prepare_ns);

    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_pending_member_resolution_duration,
        summary.g0_graph_pending_member_resolution_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_live_typeref_validation_duration,
        summary.g0_graph_live_typeref_validation_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_canonical_typeref_rebuild_duration,
        summary.g0_graph_canonical_typeref_rebuild_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_string_validation_duration,
        summary.g0_graph_string_validation_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_definition_scan_duration,
        summary.g0_graph_definition_scan_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_definition_materialization_duration,
        summary.g0_graph_definition_materialization_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_rebuild_storage_duration,
        summary.g0_graph_rebuild_storage_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_dependency_index_duration,
        summary.g0_graph_dependency_index_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_graph_final_prepare_duration,
        summary.g0_graph_final_prepare_ns);

    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_string_retention_duration,
        summary.g0_tx_string_retention_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_string_compaction_duration,
        summary.g0_tx_string_compaction_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_contribution_prepare_duration,
        summary.g0_tx_contribution_prepare_ns);

    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_source_publish_duration,
        summary.g0_tx_source_publish_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_string_publish_duration,
        summary.g0_tx_string_publish_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_contribution_publish_duration,
        summary.g0_tx_contribution_publish_ns);
    record_duration_ns(
        metrics,
        metric_id::frontend_g0_tx_graph_publish_duration,
        summary.g0_tx_graph_publish_ns);
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
    frontend_cache.invalidate();
    change_tracker.stop();
    active_configuration_path.clear();
    frontend_summary = {};
    change_tracking_ready = false;
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

    metrics.increment(metric_id::project_load_count);

    scoped_timer project_load_timer{
        metrics,
        metric_id::project_load_duration
    };

    diagnostic_records.clear();
    frontend_cache.invalidate();
    change_tracker.stop();
    active_configuration_path.clear();
    frontend_summary = {};
    change_tracking_ready = false;
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

    result =
        transaction.sources().reserve_initial_sources(
            configuration.project.size());

    if (!result.ok()) {
        try_emit(
            diagnostic_records,
            diagnostics::source_initialization_failed,
            operation,
            "while reserving initial Source capacity");

        return result;
    }

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

    source_rebuild_result rebuilt;

    metrics.increment(metric_id::frontend_build_count);

    {
        scoped_timer frontend_timer{
            metrics,
            metric_id::frontend_build_duration
        };

        rebuilt = frontend.rebuild(
            operation,
            diagnostic_records,
            acquisition,
            builder);
    }

    acquisition.flush_to(metrics);
    frontend_summary = rebuilt.frontend;

    flush_g0_frontend_metrics(
        metrics,
        frontend_summary);

    if (!rebuilt.ok()) {
        log.error(
            log_component::source,
            operation,
            "project source build failed");
        return rebuilt.semantic;
    }

    const auto cache_begin =
        std::chrono::steady_clock::now();

    const auto cache_result =
        frontend.populate_cache(frontend_cache);

    frontend_summary.g0_cache_publish_ns =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() -
                cache_begin).count());

    record_duration_ns(
        metrics,
        metric_id::frontend_cache_publish_duration,
        frontend_summary.g0_cache_publish_ns);

    if (!cache_result.ok()) {
        frontend_cache.invalidate();

        log.error(
            log_component::source,
            operation,
            "frontend cache initialization failed");
    }

    metrics.increment(metric_id::runtime_attach_count);

    {
        scoped_timer runtime_timer{
            metrics,
            metric_id::runtime_attach_duration
        };

        result = runtime_instance.attach(
            graphs.compiled_graph());
    }

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

    try {
        active_configuration_path =
            configuration_path.lexically_normal();
    }
    catch (...) {
        active_configuration_path.clear();
        frontend_cache.invalidate();
    }

    if (!active_configuration_path.empty() &&
        frontend_cache.complete()) {
        const auto tracker_begin =
            std::chrono::steady_clock::now();

        const auto tracking_result =
            change_tracker.initialize(
                graphs.sources(),
                active_configuration_path);

        frontend_summary.g0_tracker_init_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() -
                    tracker_begin).count());

        record_duration_ns(
            metrics,
            metric_id::source_tracker_init_duration,
            frontend_summary.g0_tracker_init_ns);

        change_tracking_ready =
            tracking_result.ok();

        if (!tracking_result.ok()) {
            emit_tracking_warning(
                diagnostic_records,
                operation,
                diagnostics::source_change_tracking_failed,
                log,
                "Source change tracking unavailable; full reconciliation fallback enabled");
        }
    }

    runtime_attached.store(true, std::memory_order_release);

    log.info(
        log_component::project,
        operation,
        "project load completed");

    return {};
}

status project_context::rebuild_sources(
    operation_id operation,
    logger& log,
    metrics_store& metrics) noexcept {

    diagnostic_records.clear();
    frontend_summary = {};

    if (active_configuration_path.empty() ||
        !frontend_cache.complete()) {
        return {status_code::not_available};
    }

    source_change_batch batch;

    bool reconcile_all =
        !change_tracking_ready ||
        graphs.state() != project_state::valid;

    if (change_tracking_ready) {
        const auto tracking_result =
            change_tracker.drain(batch);

        if (!tracking_result.ok() ||
            batch.rescan_required) {
            reconcile_all = true;

            emit_tracking_warning(
                diagnostic_records,
                operation,
                diagnostics::source_change_tracking_rescan,
                log,
                "Source change continuity lost; full reconciliation started");
        }

        if (batch.project_configuration_changed) {
            log.info(
                log_component::project,
                operation,
                "project configuration changed; full Project reload started");

            return load_project(
                active_configuration_path,
                operation,
                log,
                metrics);
        }

        if (!reconcile_all &&
            batch.dirty_sources.empty()) {
            return {};
        }
    }

    runtime_attached.store(false, std::memory_order_release);

    log.info(
        log_component::project,
        operation,
        reconcile_all
            ? "Source reconciliation started"
            : "incremental source rebuild started");

    auto transaction =
        graphs.begin_build(graph_build_mode::incremental);

    project_builder builder;
    source_acquisition_telemetry acquisition{metrics.mode()};
    source_frontend_generation frontend{
        transaction,
        frontend_cache
    };

    const auto rebuilt = frontend.rebuild_incremental(
        operation,
        diagnostic_records,
        acquisition,
        builder,
        batch.dirty_sources,
        reconcile_all);

    acquisition.flush_to(metrics);
    frontend_summary = rebuilt.frontend;

    if (!rebuilt.ok()) {
        log.error(
            log_component::source,
            operation,
            "incremental source rebuild failed");
        return rebuilt.semantic;
    }

    auto result = runtime_instance.attach(
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

    if (change_tracking_ready) {
        const auto tracking_result =
            change_tracker.synchronize(
                graphs.sources());

        if (!tracking_result.ok()) {
            change_tracking_ready = false;
            change_tracker.stop();

            emit_tracking_warning(
                diagnostic_records,
                operation,
                diagnostics::source_change_tracking_failed,
                log,
                "Source change tracking synchronization failed; full reconciliation fallback enabled");
        }
    }

    runtime_attached.store(true, std::memory_order_release);

    log.info(
        log_component::project,
        operation,
        reconcile_all
            ? "Source reconciliation completed"
            : "incremental source rebuild completed");

    return {};
}

status project_context::load_compiled_checkpoint(
    const std::filesystem::path& path,
    metrics_store& metrics) noexcept {

    runtime_attached.store(false, std::memory_order_release);
    frontend_cache.invalidate();
    change_tracker.stop();
    active_configuration_path.clear();
    frontend_summary = {};
    change_tracking_ready = false;

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
    frontend_cache.invalidate();
    change_tracker.stop();
    active_configuration_path.clear();
    frontend_summary = {};
    change_tracking_ready = false;
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
    frontend_cache.invalidate();
    change_tracker.stop();
    active_configuration_path.clear();
    frontend_summary = {};
    change_tracking_ready = false;

    return graphs.load_source_checkpoint(path);
}

} // namespace cw::server
