#pragma once

#include "project_configuration.hpp"
#include "../diagnostics/diagnostic_buffer.hpp"
#include "../operation.hpp"
#include "../status.hpp"
#include "../metrics/metrics_store.hpp"

#include <filesystem>
#include <string_view>

namespace cw::server
{

[[nodiscard]] status load_project_configuration(
    std::string_view text, const std::filesystem::path& configuration_path,
    operation_id operation, diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    project_configuration& output) noexcept;

[[nodiscard]] status load_project_configuration_file(
    const std::filesystem::path& configuration_path, operation_id operation,
    diagnostic_buffer& diagnostics, metrics_store& metrics,
    project_configuration& output) noexcept;

} // namespace cw::server
