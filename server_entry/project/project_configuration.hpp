#pragma once

#include "project_root.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cw::server
{

struct project_item_configuration
{
    std::filesystem::path path;
    project_item_role role = project_item_role::source;
};

enum class abi_target : std::uint8_t { windows_x64 };

struct abi_configuration
{
    abi_target target = abi_target::windows_x64;
    std::uint32_t pack = 8;
};

struct project_configuration
{
    std::uint32_t version = 0;
    std::string name;
    std::vector<project_item_configuration> project;
    abi_configuration abi;
};

inline constexpr std::uint32_t current_project_configuration_version = 1;

} // namespace cw::server
