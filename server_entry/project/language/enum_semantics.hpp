#pragma once

#include <cstdint>

namespace cw::server
{

enum class enum_definition_state : std::uint8_t
{
    opaque,
    defined
};

} // namespace cw::server
