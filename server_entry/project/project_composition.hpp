#pragma once

#include <cstddef>

namespace cw::server {

// Captures execution statistics for one Project composition resolution.
// These values describe resolver work and parallelism for that operation and
// are separate from the process-wide metrics_store telemetry aggregates.
struct project_composition_statistics {
    std::size_t worker_limit = 0;
    std::size_t max_active_workers = 0;
    std::size_t configuration_files_loaded = 0;
};

} // namespace cw::server
