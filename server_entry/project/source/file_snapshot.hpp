#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace cw::server { class source_acquisition_timing; }

namespace cw::server::core
{
struct file_snapshot_observation
{
    // Canonical Windows FILETIME: 100 ns ticks since 1601-01-01 UTC.
    std::uint64_t write_time_ticks = 0;
    std::uintmax_t size = 0;
    friend bool operator==(const file_snapshot_observation&,
                           const file_snapshot_observation&) noexcept = default;
};

enum class file_snapshot_result : std::uint8_t
{
    acquired,
    unchanged,
    missing,
    changed_during_read,
    failed,
    allocation_failed
};

struct file_snapshot
{
    file_snapshot_observation observation;
    std::string bytes;
};

[[nodiscard]] file_snapshot_result acquire_file_snapshot(
    const std::filesystem::path& path,
    const std::optional<file_snapshot_observation>& baseline,
    file_snapshot& output,
    source_acquisition_timing& timing) noexcept;
} // namespace cw::server::core
