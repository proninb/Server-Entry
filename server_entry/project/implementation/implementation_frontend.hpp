#pragma once

#include "implementation_facts.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

#include <span>
#include <vector>

namespace cw::server {

class diagnostic_buffer;
class graph;
class source_manager;
class source_manager_update;
class string_registry;

// Coordinates post-G0 implementation parsing. It reads committed canonical
// type state and either committed or candidate Source bytes, then publishes
// fully resolved Source-local facts for Runtime materialization.
class implementation_frontend final {
public:
    [[nodiscard]] status build(
        const source_manager& sources,
        const graph& graph,
        const string_registry& strings,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        std::vector<implementation_facts_storage>& output) const noexcept;

    [[nodiscard]] status rebuild(
        const source_manager_update& sources,
        std::span<const source_id> dirty_sources,
        const graph& graph,
        const string_registry& strings,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        std::vector<implementation_facts_storage>& output) const noexcept;
};

} // namespace cw::server
