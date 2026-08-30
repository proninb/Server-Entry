#pragma once

#include "../../status.hpp"

namespace cw::server
{

class shared_memory
{
public:
    [[nodiscard]] status initialize() noexcept;
};

} // namespace cw::server
