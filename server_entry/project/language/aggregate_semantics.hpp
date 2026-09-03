#pragma once

#include <cstdint>

namespace cw::server
{
enum class aggregate_definition_state : std::uint8_t
{
    declared,
    defined
};
}
