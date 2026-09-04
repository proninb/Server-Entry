#pragma once

#include "../../status.hpp"

#include <filesystem>

namespace cw::server {

class graph;
class metrics_store;
class string_registry;

// Persists the committed compiled Project state as one validated binary artifact.
// The artifact contains the dense String Registry projection and canonical Graph
// export required to restore a runnable compiled state; Source Manager state is
// persisted independently by its own checkpoint contract.
[[nodiscard]] status write_compiled_checkpoint(
    const std::filesystem::path& path,
    const string_registry& registry,
    const graph& graph_state,
    metrics_store* metrics = nullptr) noexcept;

// Restores one compiled artifact into an empty String Registry and Graph target.
// Validation is completed before either destination is mutated.
[[nodiscard]] status read_compiled_checkpoint(
    const std::filesystem::path& path,
    string_registry& registry,
    graph& graph_state,
    metrics_store* metrics = nullptr) noexcept;

} // namespace cw::server
