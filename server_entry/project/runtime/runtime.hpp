#pragma once

#include "../../status.hpp"

namespace cw::server {

class graph;

// Owns the execution-facing Runtime state for one Project.
// Runtime consumes an already committed canonical Graph and prepares the state
// required for task execution; Graph construction and publication remain outside
// this layer.
class runtime {
public:
    [[nodiscard]] status attach(const graph& graph) noexcept;
};

} // namespace cw::server
