#pragma once

#include "project_composition.hpp"
#include "project_configuration.hpp"
#include "project_root.hpp"
#include "../diagnostics/diagnostic_buffer.hpp"
#include "../metrics/metrics_store.hpp"
#include "../operation.hpp"
#include "../status.hpp"

#include <filesystem>

namespace cw::server {

// Resolves the complete Project composition rooted at one validated configuration.
// Nested project configurations are loaded and deduplicated, composition cycles
// are rejected, and resolved non-project items are published through project_root_sink.
[[nodiscard]] status resolve_project_composition(
    const std::filesystem::path& root_configuration_path,
    const project_configuration& root_configuration,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    project_root_sink& roots,
    project_composition_statistics* statistics = nullptr) noexcept;

} // namespace cw::server
