#pragma once

#include "token.hpp"
#include "../../status.hpp"
#include "../source/source_view.hpp"

#include <vector>

namespace cw::server {

class diagnostic_buffer;
class operation_id;

// Identifies the half-open token range occupied by one preprocessing directive.
// The range refers to entries in the token sequence produced by lex_source().
struct directive_span {
    std::uint32_t token_begin = 0;
    std::uint32_t token_end = 0;
};

// Performs lexical analysis over one immutable Source snapshot.
// Tokens contain source-local byte offsets and never own or retain Source bytes.
// When requested, preprocessing directives are reported as token ranges without
// interpreting their language semantics.
[[nodiscard]] status lex_source(
    source_view source,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<parser_token>& output,
    std::vector<directive_span>* directives = nullptr) noexcept;

} // namespace cw::server
