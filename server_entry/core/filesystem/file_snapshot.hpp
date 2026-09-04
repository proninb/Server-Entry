#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace cw::server {

class source_acquisition_timing;

} // namespace cw::server

namespace cw::server::core {

// Captures the filesystem metadata used to detect whether a Source file changed.
// write_time_ticks uses one canonical representation on all platforms:
// 100 ns ticks since 1601-01-01 UTC, matching Windows FILETIME units.
struct file_snapshot_observation {
    std::uint64_t write_time_ticks = 0;
    std::uintmax_t size = 0;

    friend bool operator==(const file_snapshot_observation&,
                           const file_snapshot_observation&) noexcept = default;
};

// Describes the outcome of one file snapshot acquisition attempt.
// It distinguishes normal change-detection outcomes from I/O and allocation failures.
enum class file_snapshot_result : std::uint8_t {
    acquired,
    unchanged,
    missing,
    changed_during_read,
    failed,
    allocation_failed
};

// Owns one successfully acquired file image together with the filesystem
// observation that describes the file at acquisition time.
struct file_snapshot {
    file_snapshot_observation observation;
    std::string bytes;
};

// Acquires a consistent file snapshot and compares it with an optional prior observation.
// The operation also records Source acquisition timing for the caller.
[[nodiscard]] file_snapshot_result acquire_file_snapshot(
    const std::filesystem::path& path,
    const std::optional<file_snapshot_observation>& baseline,
    file_snapshot& output,
    source_acquisition_timing& timing) noexcept;

} // namespace cw::server::core
