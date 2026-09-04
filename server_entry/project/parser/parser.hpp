#pragma once

#include "source_context.hpp"
#include "source_environment.hpp"
#include "token.hpp"
#include "language_configuration.hpp"
#include "../source/source_manager.hpp"
#include "../../operation.hpp"

#include <span>

namespace cw::server {

// Parses one immutable Source snapshot using the default native frontend.
// The operation performs lexical analysis first and writes transient parser
// facts and diagnostics into source_context; it does not publish canonical G.
[[nodiscard]] status parse_source(
    source_view source,
    const source_environment& environment,
    operation_id operation,
    source_context& context) noexcept;

// Parses an already tokenized Source snapshot.
// Token storage and Source bytes remain caller-owned for the duration of parsing.
[[nodiscard]] status parse_source_tokens(
    source_view source,
    std::span<const parser_token> tokens,
    const source_environment& environment,
    operation_id operation,
    source_context& context) noexcept;

// Defines the language-parser boundary used by Parser orchestration.
// A backend consumes Source-local tokens and source_environment lookup services
// and produces transient source_context facts; canonical identity and Graph
// publication remain outside the backend.
class parser_backend {
public:
    virtual ~parser_backend() = default;

    [[nodiscard]] virtual status parse(
        source_view source,
        std::span<const parser_token> tokens,
        const source_environment& environment,
        const language_configuration& language,
        operation_id operation,
        source_context& context) const noexcept = 0;
};

// Implements the built-in native parser for the currently supported language subset.
class native_parser_backend final : public parser_backend {
public:
    [[nodiscard]] status parse(
        source_view source,
        std::span<const parser_token> tokens,
        const source_environment& environment,
        const language_configuration& language,
        operation_id operation,
        source_context& context) const noexcept override;
};

// Returns the process-wide stateless native parser backend.
[[nodiscard]] const parser_backend& default_parser_backend() noexcept;

} // namespace cw::server
