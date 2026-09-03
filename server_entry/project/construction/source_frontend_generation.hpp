#pragma once

#include "../parser/lexer.hpp"
#include "../parser/language_configuration.hpp"
#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <memory>
#include <optional>
#include <vector>

namespace cw::server
{
class diagnostic_buffer;
class graph_build_transaction;
class operation_id;
class project_builder;
class parser_backend;
class source_context;
class source_environment;
class source_acquisition_telemetry;
class metrics_store;

struct source_rebuild_result
{
    status semantic{};
    std::optional<status> checkpoint;
    std::optional<status> compiled_checkpoint;
    source_rebuild_result() = default;
    source_rebuild_result(status result) noexcept : semantic(result) {}
    [[nodiscard]] bool ok() const noexcept { return semantic.ok(); }
};

struct source_frontend_counts
{
    std::uint32_t discovery = 0;
    std::uint32_t lex = 0;
    std::uint32_t parse = 0;
    std::uint32_t publish = 0;
};

class source_frontend_generation final
{
public:
    explicit source_frontend_generation(
        graph_build_transaction& transaction,
        language_configuration language = {}) noexcept;
    source_frontend_generation(graph_build_transaction& transaction,
        const parser_backend& backend,
        language_configuration language = {}) noexcept;
    [[nodiscard]] status enqueue(source_id source) noexcept;
    [[nodiscard]] bool take_discovery(source_id& source) noexcept;
    [[nodiscard]] status discover(source_id source, operation_id operation,
                                  diagnostic_buffer& diagnostics) noexcept;
    [[nodiscard]] bool take_semantic_ready(source_id& source) noexcept;
    [[nodiscard]] status parse_and_publish(source_id source,
        operation_id operation,
        source_context& context, const project_builder& builder) noexcept;
    [[nodiscard]] status finish_discovery(operation_id operation,
                                          diagnostic_buffer& diagnostics) noexcept;
    [[nodiscard]] source_rebuild_result rebuild(operation_id operation,
        diagnostic_buffer& diagnostics, source_acquisition_telemetry& telemetry,
        const project_builder& builder,
        const std::filesystem::path& checkpoint = {},
        metrics_store* checkpoint_metrics = nullptr,
        const std::filesystem::path& compiled_checkpoint = {}) noexcept;
    [[nodiscard]] source_frontend_counts counts(source_id source) const noexcept;
    [[nodiscard]] std::uint32_t remaining_dependencies(source_id source) const noexcept;
    [[nodiscard]] bool published(source_id source) const noexcept;
    [[nodiscard]] const source_environment_storage* interface(source_id source) const noexcept;
    [[nodiscard]] bool failed() const noexcept;

private:
    struct source_state
    {
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
    void fail_locked(status result) noexcept;
    void enqueue_ready_locked(source_id source, source_state& state);

    graph_build_transaction* transaction_ = nullptr;
    const parser_backend* backend_ = nullptr;
    language_configuration language_;
    mutable std::mutex mutex_;
    std::mutex publication_mutex_;
    std::vector<source_state> states_;
    std::deque<source_id> discovery_queue_;
    std::deque<source_id> semantic_queue_;
    std::uint32_t active_discoveries_ = 0;
    status failure_{};
};
}
