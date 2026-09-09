#pragma once

#include "implementation_facts.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

#include <span>
#include <vector>

namespace cw::server {

class diagnostic_buffer;
class graph_type_view;
class source_manager;
class source_manager_update;
class string_registry;

// Coordinates implementation parsing against one complete canonical Type
// domain. Type semantics are read only through graph_type_view; Source bytes
// may be committed or candidate and the resulting facts remain Source-local.
class implementation_frontend final {
public:
    [[nodiscard]] status build(
        const source_manager& sources,
        const graph_type_view& types,
        const string_registry& strings,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        std::vector<implementation_facts_storage>& output) const noexcept;

    [[nodiscard]] status rebuild(
        const source_manager_update& sources,
        std::span<const source_id> dirty_sources,
        const graph_type_view& types,
        const string_registry& strings,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        std::vector<implementation_facts_storage>& output) const noexcept;
};

} // namespace cw::server
