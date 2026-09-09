#pragma once

#include "implementation_context.hpp"
#include "../source/source_view.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

namespace cw::server {

class graph;
class string_registry;

// Parses one implementation Source against an already committed G0.
// All type/member/object relations in the resulting facts are resolved; Runtime
// never receives unresolved source-language names.
[[nodiscard]] status parse_implementation_source(
    source_view source,
    const graph& graph,
    const string_registry& strings,
    operation_id operation,
    implementation_context& context) noexcept;

} // namespace cw::server
