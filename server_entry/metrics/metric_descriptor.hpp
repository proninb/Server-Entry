#pragma once

#include "metric.hpp"

#include <string_view>

namespace cw::server
{

struct metric_descriptor
{
    metric_id id;
    metric_kind kind;
    std::string_view name;
    std::string_view description;
};

} // namespace cw::server
