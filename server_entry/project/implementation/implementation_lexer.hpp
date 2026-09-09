#pragma once

#include "implementation_token.hpp"
#include "../source/source_view.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

#include <vector>

namespace cw::server {

class diagnostic_buffer;

// Lexes implementation Sources independently from the declaration frontend.
// The token stream contains only syntax needed by object declarations,
// initialization and resolved object/member paths.
[[nodiscard]] status lex_implementation_source(
    source_view source,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<implementation_token>& output) noexcept;

} // namespace cw::server
