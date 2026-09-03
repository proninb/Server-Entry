#include "file_snapshot.hpp"

#include "../../metrics/source_acquisition_telemetry.hpp"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server::core
{
namespace
{
class unique_handle
{
public:
    explicit unique_handle(HANDLE value) noexcept : value_(value) {}
    ~unique_handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    void close() noexcept
    {
        if (value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
            value_ = INVALID_HANDLE_VALUE;
        }
    }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

std::uint64_t file_time_ticks(const FILETIME& value) noexcept
{
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
           value.dwLowDateTime;
}

bool observe(HANDLE handle, file_snapshot_observation& output) noexcept
{
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    output.size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
                  information.nFileSizeLow;
    output.write_time_ticks = file_time_ticks(information.ftLastWriteTime);
    return true;
}

} // namespace

file_snapshot_result acquire_file_snapshot(
    const std::filesystem::path& path,
    const std::optional<file_snapshot_observation>& baseline,
    file_snapshot& output, source_acquisition_timing& timing) noexcept
{
    output = {};
    const auto opened = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    timing.finish_phase(metric_id::source_file_open_duration);
    unique_handle handle{opened};
    if (handle.get() == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? file_snapshot_result::missing
                   : file_snapshot_result::failed;
    }

    const auto observed = observe(handle.get(), output.observation);
    timing.finish_phase(metric_id::source_observe_before_duration);
    if (!observed)
    {
        handle.close();
        timing.advance_boundary();
        return file_snapshot_result::failed;
    }
    if (baseline && *baseline == output.observation)
    {
        handle.close();
        timing.advance_boundary();
        return file_snapshot_result::unchanged;
    }
    if (output.observation.size > std::numeric_limits<std::size_t>::max())
    {
        handle.close();
        timing.advance_boundary();
        return file_snapshot_result::allocation_failed;
    }

    try { output.bytes.resize(static_cast<std::size_t>(output.observation.size)); }
    catch (const std::bad_alloc&)
    {
        handle.close();
        timing.advance_boundary();
        return file_snapshot_result::allocation_failed;
    }
    catch (const std::length_error&)
    {
        handle.close();
        timing.advance_boundary();
        return file_snapshot_result::allocation_failed;
    }

    std::size_t offset = 0;
    while (offset < output.bytes.size())
    {
        const auto remaining = output.bytes.size() - offset;
        const auto requested = static_cast<DWORD>((std::min)(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD received = 0;
        if (!ReadFile(handle.get(), output.bytes.data() + offset, requested, &received, nullptr) ||
            received == 0)
        {
            timing.finish_phase(metric_id::source_read_duration);
            handle.close();
            timing.advance_boundary();
            return file_snapshot_result::failed;
        }
        offset += received;
    }
    timing.finish_phase(metric_id::source_read_duration);

    file_snapshot_observation after;
    const auto observed_after = observe(handle.get(), after);
    timing.finish_phase(metric_id::source_observe_after_duration);
    handle.close();
    timing.advance_boundary();
    if (!observed_after) return file_snapshot_result::failed;
    if (after != output.observation) return file_snapshot_result::changed_during_read;
    return file_snapshot_result::acquired;
}
} // namespace cw::server::core
