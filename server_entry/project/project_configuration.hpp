#pragma once

#include "project_root.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cw::server {

// Describes one item declared by the Project configuration.
// Paths are resolved and normalized by the configuration loader before the
// resulting project_configuration is published to project composition.
struct project_item_configuration {
    std::filesystem::path path;
    project_item_role role = project_item_role::source;
};

// Identifies the target ABI used to build canonical Project layout.
// The target is explicit and independent of the operating system hosting Server.
// Numeric values are persisted and therefore form part of the artifact contract.
enum class abi_target : std::uint8_t {
    windows_x64 = 0,
    posix_x64 = 1
};

[[nodiscard]] constexpr bool is_supported_abi_target(
    abi_target target) noexcept {

    return
        target == abi_target::windows_x64 ||
        target == abi_target::posix_x64;
}

[[nodiscard]] constexpr bool is_supported_abi_pack(
    std::uint32_t pack) noexcept {

    return
        pack == 1 ||
        pack == 2 ||
        pack == 4 ||
        pack == 8 ||
        pack == 16;
}

// Defines the ABI settings that govern canonical type layout for the Project.
// windows_x64 uses the Windows LLP64 data model; posix_x64 uses the project
// POSIX x64 LP64 data model. These values are part of the Graph build contract.
struct abi_configuration {
    abi_target target = abi_target::windows_x64;
    std::uint32_t pack = 8;
};

[[nodiscard]] constexpr bool is_supported_abi_configuration(
    const abi_configuration& abi) noexcept {

    return
        is_supported_abi_target(abi.target) &&
        is_supported_abi_pack(abi.pack);
}

// Represents the validated, typed contents of one Project configuration.
// It carries project identity, composition inputs, and ABI settings after the
// loader has resolved configuration-relative paths.
struct project_configuration {
    std::uint32_t version = 0;
    std::string name;
    std::vector<project_item_configuration> project;
    abi_configuration abi;
};

inline constexpr std::uint32_t current_project_configuration_version = 1;

} // namespace cw::server
