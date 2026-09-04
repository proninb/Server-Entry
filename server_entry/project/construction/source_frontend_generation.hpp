#pragma once

#include "../parser/language_configuration.hpp"
#include "../parser/lexer.hpp"
#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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

// Reports semantic build status separately from optional persistence work.
// A successfully committed Graph remains semantically successful even when an
// optional Source Manager or compiled checkpoint save subsequently fails.
struct source_rebuild_result {
    status semantic{};
    std::optional<status> checkpoint;
    std::optional<status> compiled_checkpoint;

    source_rebuild_result() = default;

    source_rebuild_result(status result) noexcept
        : semantic(result) {}

    [[nodiscard]] bool ok() const noexcept {
        return semantic.ok();
    }
};

// Counts frontend work performed for one Source during this generation.
struct source_frontend_counts {
    std::uint32_t discovery = 0;
    std::uint32_t lex = 0;
    std::uint32_t parse = 0;
    std::uint32_t publish = 0;
};

// Coordinates one Source frontend generation inside an active Graph build
// transaction. Discovery acquires/lexes Sources and materializes the dependency
// graph; semantic work becomes eligible only after all direct dependencies have
// published their Parser-visible interfaces.
//
// Parsing may run outside the orchestration mutex, while publication into the
// shared Graph build transaction is serialized by publication_mutex. The class
// owns only generation-local scheduling state and Parser interfaces; canonical
// identity and Graph publication remain Builder/Graph responsibilities.
class source_frontend_generation final {
public:
    explicit source_frontend_generation(
        graph_build_transaction& transaction,
        language_configuration language = {}) noexcept;

    source_frontend_generation(
        graph_build_transaction& transaction,
        const parser_backend& backend,
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

    [[nodiscard]] status parse_and_publish(
        source_id source,
        operation_id operation,
        source_context& context,
        const project_builder& builder) noexcept;

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

    [[nodiscard]] source_frontend_counts counts(
        source_id source) const noexcept;

    [[nodiscard]] std::uint32_t remaining_dependencies(
        source_id source) const noexcept;

    [[nodiscard]] bool published(
        source_id source) const noexcept;

    // Returns the immutable Parser-visible interface exported by a Source after
    // publication. The pointer remains owned by this frontend generation.
    [[nodiscard]] const source_environment_storage* interface(
        source_id source) const noexcept;

    [[nodiscard]] bool failed() const noexcept;

private:
    // Per-Source generation state. tokens excludes preprocessing directives and
    // is retained until semantic parsing; interface becomes immutable once the
    // Source has been published.
    struct source_state {
        std::vector<parser_token> tokens;
        std::vector<source_id> dependencies;
        std::vector<source_id> dependents;
        std::unique_ptr<source_environment_storage> interface;

        bool discovery_claimed = false;
        bool discovery_done = false;
        bool parse_claimed = false;
        bool published = false;
        bool semantic_queued = false;

        std::uint32_t remaining = 0;
        source_frontend_counts counts;
    };

    source_state& ensure(source_id source);

    // Requires mutex to be held. First failure wins and cancels the coordinated
    // Graph build transaction.
    void fail_locked(status result) noexcept;

    // Requires mutex to be held. A Source is semantically ready only after
    // discovery completes and every dependency has published its interface.
    void enqueue_ready_locked(
        source_id source,
        source_state& state);

    graph_build_transaction* transaction = nullptr;
    const parser_backend* backend = nullptr;
    language_configuration language{};

    mutable std::mutex mutex;

    // Builder/Graph mutation is single-writer even when parsing is scheduled
    // concurrently.
    std::mutex publication_mutex;

    std::vector<source_state> states;
    std::deque<source_id> discovery_queue;
    std::deque<source_id> semantic_queue;

    std::uint32_t active_discoveries = 0;
    status failure{};
};

} // namespace cw::server
