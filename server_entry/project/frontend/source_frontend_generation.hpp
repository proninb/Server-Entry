#pragma once

#include "../builder/source_build_entry.hpp"
#include "../parser/language_configuration.hpp"
#include "../parser/lexer.hpp"
#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace cw::server {

class diagnostic_buffer;
class graph_build_transaction;
class metrics_store;
class operation_id;
class parser_backend;
class project_builder;
class source_acquisition_telemetry;
class source_context;
class source_environment;
class source_frontend_cache;

struct source_frontend_summary {
    std::uint32_t dirty = 0;
    std::uint32_t checked = 0;
    std::uint32_t affected = 0;
    std::uint32_t discovery = 0;
    std::uint32_t lex = 0;
    std::uint32_t parse = 0;
    std::uint32_t publish = 0;
    bool reconciliation = false;
};

// Reports semantic build status separately from optional persistence work.
// A successfully committed Graph remains semantically successful even when an
// optional Source Manager or compiled checkpoint save subsequently fails.
struct source_rebuild_result {
    status semantic{};
    std::optional<status> checkpoint;
    std::optional<status> compiled_checkpoint;
    source_frontend_summary frontend;

    source_rebuild_result() = default;

    source_rebuild_result(status result) noexcept
        : semantic(result) {}

    [[nodiscard]] bool ok() const noexcept {
        return semantic.ok();
    }
};

struct source_frontend_counts {
    std::uint32_t discovery = 0;
    std::uint32_t lex = 0;
    std::uint32_t parse = 0;
    std::uint32_t publish = 0;
};

// Coordinates one Source frontend generation inside an active Graph build
// transaction. G0 performs the full build. Incremental generations reuse
// committed COLD interfaces and parse only dirty Sources plus dependents.
class source_frontend_generation final {
public:
    explicit source_frontend_generation(
        graph_build_transaction& transaction,
        language_configuration language = {}) noexcept;

    source_frontend_generation(
        graph_build_transaction& transaction,
        const parser_backend& backend,
        language_configuration language = {}) noexcept;

    source_frontend_generation(
        graph_build_transaction& transaction,
        source_frontend_cache& cache,
        language_configuration language = {}) noexcept;

    [[nodiscard]] status enqueue(source_id source) noexcept;

    [[nodiscard]] bool take_discovery(
        source_id& source) noexcept;

    [[nodiscard]] status discover(
        source_id source,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    [[nodiscard]] bool take_semantic_ready(
        source_id& source) noexcept;

    [[nodiscard]] status parse_and_capture(
        source_id source,
        operation_id operation,
        source_context& context) noexcept;

    [[nodiscard]] status finish_discovery(
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    [[nodiscard]] source_rebuild_result rebuild(
        operation_id operation,
        diagnostic_buffer& diagnostics,
        source_acquisition_telemetry& telemetry,
        const project_builder& builder,
        const std::filesystem::path& checkpoint = {},
        metrics_store* checkpoint_metrics = nullptr,
        const std::filesystem::path& compiled_checkpoint = {}) noexcept;

    // Normal path observes only dirty Sources. reconcile_all is the fail-closed
    // fallback used after watcher overflow/loss or recovery from a failed build.
    [[nodiscard]] source_rebuild_result rebuild_incremental(
        operation_id operation,
        diagnostic_buffer& diagnostics,
        source_acquisition_telemetry& telemetry,
        const project_builder& builder,
        std::span<const source_id> dirty_sources,
        bool reconcile_all = false) noexcept;

    [[nodiscard]] status populate_cache(
        source_frontend_cache& destination) noexcept;

    [[nodiscard]] source_frontend_counts counts(
        source_id source) const noexcept;

    [[nodiscard]] source_frontend_summary summary() const noexcept;

    [[nodiscard]] std::uint32_t remaining_dependencies(
        source_id source) const noexcept;

    [[nodiscard]] bool published(
        source_id source) const noexcept;

    [[nodiscard]] const source_environment_storage* interface(
        source_id source) const noexcept;

    [[nodiscard]] bool failed() const noexcept;

private:
    struct include_visibility {
        std::uint32_t visible_from = 0;
        source_id dependency{};
    };

    struct source_state {
        std::vector<parser_token> tokens;
        std::vector<source_id> dependencies;
        std::vector<include_visibility> visibility;
        std::vector<source_id> dependents;
        std::unique_ptr<source_environment_storage> interface;
        const source_environment_storage* cached_interface = nullptr;
        std::unique_ptr<source_build_entry> build_entry;

        bool discovery_claimed = false;
        bool discovery_done = false;
        bool parse_claimed = false;
        bool parsed = false;
        bool published = false;
        bool semantic_queued = false;
        bool removed = false;

        std::uint32_t remaining = 0;
        source_frontend_counts counts;
    };

    source_state& ensure(source_id source);

    void fail_locked(status result) noexcept;

    void enqueue_ready_locked(
        source_id source,
        source_state& state);

    graph_build_transaction* transaction = nullptr;
    const parser_backend* backend = nullptr;
    source_frontend_cache* cache = nullptr;
    language_configuration language{};

    mutable std::mutex mutex;

    std::condition_variable discovery_condition;
    std::condition_variable semantic_condition;

    bool semantic_scheduler_active = false;
    std::uint32_t semantic_remaining = 0;

    std::vector<source_state> states;
    std::deque<source_id> discovery_queue;
    std::deque<source_id> semantic_queue;

    std::uint32_t active_discoveries = 0;
    source_frontend_summary current_summary;
    status failure{};
};

} // namespace cw::server
