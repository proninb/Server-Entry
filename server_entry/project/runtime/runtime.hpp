#pragma once

#include "../../status.hpp"

namespace cw::server
{

class graph;

class runtime
{
public:
    [[nodiscard]] status attach(const graph& graph) noexcept;
};

} // namespace cw::server
