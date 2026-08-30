#pragma once

#include "project_composition.hpp"
#include "../diagnostics/diagnostic_buffer.hpp"
#include "../operation.hpp"
#include "../status.hpp"
#include "../metrics/metrics_store.hpp"

#include <filesystem>

namespace cw::server
{

[[nodiscard]] status resolve_project_composition(
    const std::filesystem::path& root_configuration_path,
    const project_configuration& root_configuration,
    operation_id operation, diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    resolved_project_composition& output,
    project_composition_statistics* statistics = nullptr) noexcept;

} // namespace cw::server
