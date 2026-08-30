#pragma once

#include "project_configuration.hpp"

#include <filesystem>
#include <vector>

namespace cw::server
{

// Project references are graph edges and are not published as effective items.
// Each reachable project contributes its type/source items once, in first DFS
// encounter order following declaration order.
struct resolved_project_item
{
    std::filesystem::path path;
    project_item_role role = project_item_role::source;
};

struct resolved_project_composition
{
    std::vector<resolved_project_item> items;
};

struct project_composition_statistics
{
    std::size_t worker_limit = 0;
    std::size_t max_active_workers = 0;
    std::size_t configuration_files_loaded = 0;
};

} // namespace cw::server
