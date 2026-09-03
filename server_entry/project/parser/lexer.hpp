#pragma once

#include "token.hpp"
#include "../../status.hpp"
#include "../source/source_view.hpp"

#include <vector>

namespace cw::server
{
class diagnostic_buffer;
class operation_id;

struct directive_span
{
    std::uint32_t token_begin = 0;
    std::uint32_t token_end = 0;
};

// Lexical-only operation over one immutable Source snapshot. Tokens are
// source-local offsets and never own or retain source bytes.
[[nodiscard]] status lex_source(source_view source, operation_id operation,
                                diagnostic_buffer& diagnostics,
                                std::vector<parser_token>& output,
                                std::vector<directive_span>* directives = nullptr) noexcept;
}
