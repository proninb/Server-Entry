#pragma once

#include "../diagnostics/diagnostic_buffer.hpp"
#include "../metrics/metrics_store.hpp"
#include "../operation.hpp"
#include "../status.hpp"
#include "project_configuration.hpp"

#include <filesystem>
#include <string_view>

namespace cw::server {

// Loads and validates one Project configuration while preserving diagnostics
// and metrics as explicit orchestration outputs.
[[nodiscard]] status load_project_configuration(
    std::string_view text,
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    project_configuration& output) noexcept;

// Loads one Project configuration file through the same transactional path.
[[nodiscard]] status load_project_configuration_file(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    project_configuration& output) noexcept;

}