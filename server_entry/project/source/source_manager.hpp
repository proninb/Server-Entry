#pragma once

#include "../../status.hpp"

namespace cw::server
{

class source_manager
{
public:
    [[nodiscard]] status initialize() noexcept;
};

} // namespace cw::server
