#pragma once

#include "../../source_id.hpp"

#include <string_view>

namespace cw::server
{
struct source_view
{
    source_id source;
    std::string_view bytes;
};
}
