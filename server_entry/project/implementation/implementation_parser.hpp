#pragma once

#include "implementation_context.hpp"
#include "../source/source_view.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

namespace cw::server {

class graph_type_view;
class string_registry;

// Parses one implementation Source against a complete canonical Type domain.
// The supplied graph_type_view may represent a committed Graph or a sealed
// candidate; the Parser has no Graph mutation capability.
[[nodiscard]] status parse_implementation_source(
    source_view source,
    const graph_type_view& types,
    const string_registry& strings,
    operation_id operation,
    implementation_context& context) noexcept;

} // namespace cw::server
