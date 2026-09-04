#include "file_snapshot.hpp"

#include "../../metrics/source_acquisition_telemetry.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>
#include <stdexcept>

#ifdef _WIN32

#define NOMINMAX
#include <Windows.h>

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#endif

namespace cw::server::core {
namespace {

#ifdef _WIN32

// Owns one Win32 file handle and guarantees CloseHandle on every exit path.
class native_file {
public:
    explicit native_file(HANDLE value) noexcept : handle(value) {}

    ~native_file() {
        close();
    }

    native_file(const native_file&) = delete;
    native_file& operator=(const native_file&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return handle != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle;
    }

    void close() noexcept {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle = INVALID_HANDLE_VALUE;
};

bool observe(HANDLE handle, file_snapshot_observation& output) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};

    if (!GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    output.write_time_ticks =
        (static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32) |
        information.ftLastWriteTime.dwLowDateTime;
    output.size =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
        information.nFileSizeLow;

    return true;
}

native_file open_source_file(
    const std::filesystem::path& path,
    file_snapshot_result& failure) noexcept {

    const auto handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        failure = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                      ? file_snapshot_result::missing
                      : file_snapshot_result::failed;
    }

    return native_file{handle};
}

bool read_source_file(HANDLE handle, std::string& bytes) noexcept {
    std::size_t offset = 0;

    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto requested = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));

        DWORD received = 0;
        if (!ReadFile(handle, bytes.data() + offset, requested, &received, nullptr) ||
            received == 0) {
            return false;
        }

        offset += received;
    }

    return true;
}

#else

// Owns one POSIX file descriptor and guarantees close() on every exit path.
class native_file {
public:
    explicit native_file(int value) noexcept : descriptor(value) {}

    ~native_file() {
        close();
    }

    native_file(const native_file&) = delete;
    native_file& operator=(const native_file&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return descriptor >= 0;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor;
    }

    void close() noexcept {
        if (descriptor >= 0) {
            ::close(descriptor);
            descriptor = -1;
        }
    }

private:
    int descriptor = -1;
};

constexpr std::int64_t filetime_epoch_offset_seconds = 11644473600ll;
constexpr std::uint64_t filetime_ticks_per_second = 10000000ull;

bool observe(int descriptor, file_snapshot_observation& output) noexcept {
    struct stat information {};

    if (::fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_size < 0) {
        return false;
    }

#ifdef __APPLE__
    const auto seconds = information.st_mtimespec.tv_sec;
    const auto nanoseconds = information.st_mtimespec.tv_nsec;
#else
    const auto seconds = information.st_mtim.tv_sec;
    const auto nanoseconds = information.st_mtim.tv_nsec;
#endif

    if (nanoseconds < 0 || nanoseconds >= 1000000000L) {
        return false;
    }

    const auto signed_seconds = static_cast<std::int64_t>(seconds);
    if (signed_seconds < -filetime_epoch_offset_seconds) {
        return false;
    }

    const auto filetime_seconds =
        static_cast<std::uint64_t>(signed_seconds + filetime_epoch_offset_seconds);

    if (filetime_seconds >
        std::numeric_limits<std::uint64_t>::max() / filetime_ticks_per_second) {
        return false;
    }

    output.write_time_ticks =
        filetime_seconds * filetime_ticks_per_second +
        static_cast<std::uint64_t>(nanoseconds) / 100ull;
    output.size = static_cast<std::uintmax_t>(information.st_size);

    return true;
}

native_file open_source_file(
    const std::filesystem::path& path,
    file_snapshot_result& failure) noexcept {

    int flags = O_RDONLY;

#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    const auto descriptor = ::open(path.c_str(), flags);

    if (descriptor < 0) {
        const auto error = errno;
        failure = error == ENOENT || error == ENOTDIR
                      ? file_snapshot_result::missing
                      : file_snapshot_result::failed;
        return native_file{descriptor};
    }

#if defined(POSIX_FADV_SEQUENTIAL)
    (void)::posix_fadvise(descriptor, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    return native_file{descriptor};
}

bool read_source_file(int descriptor, std::string& bytes) noexcept {
    std::size_t offset = 0;

    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto requested = (std::min)(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));

        const auto received = ::read(descriptor, bytes.data() + offset, requested);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (received == 0) {
            return false;
        }

        offset += static_cast<std::size_t>(received);
    }

    return true;
}

#endif

} // namespace

file_snapshot_result acquire_file_snapshot(
    const std::filesystem::path& path,
    const std::optional<file_snapshot_observation>& baseline,
    file_snapshot& output,
    source_acquisition_timing& timing) noexcept {

    output = {};

    auto open_failure = file_snapshot_result::failed;
    auto file = open_source_file(path, open_failure);
    timing.finish_phase(metric_id::source_file_open_duration);

    if (!file.valid()) {
        return open_failure;
    }

    const auto observed = observe(file.get(), output.observation);
    timing.finish_phase(metric_id::source_observe_before_duration);

    if (!observed) {
        file.close();
        timing.advance_boundary();
        return file_snapshot_result::failed;
    }

    // Metadata equality is the fast path: unchanged Sources do not require a read.
    if (baseline && *baseline == output.observation) {
        file.close();
        timing.advance_boundary();
        return file_snapshot_result::unchanged;
    }

    if (output.observation.size > std::numeric_limits<std::size_t>::max()) {
        file.close();
        timing.advance_boundary();
        return file_snapshot_result::allocation_failed;
    }

    try {
        output.bytes.resize(static_cast<std::size_t>(output.observation.size));
    }
    catch (const std::bad_alloc&) {
        file.close();
        timing.advance_boundary();
        return file_snapshot_result::allocation_failed;
    }
    catch (const std::length_error&) {
        file.close();
        timing.advance_boundary();
        return file_snapshot_result::allocation_failed;
    }

    if (!read_source_file(file.get(), output.bytes)) {
        timing.finish_phase(metric_id::source_read_duration);
        file.close();
        timing.advance_boundary();
        return file_snapshot_result::failed;
    }

    timing.finish_phase(metric_id::source_read_duration);

    file_snapshot_observation after;
    const auto observed_after = observe(file.get(), after);

    timing.finish_phase(metric_id::source_observe_after_duration);
    file.close();
    timing.advance_boundary();

    if (!observed_after) {
        return file_snapshot_result::failed;
    }

    // A changed observation means the bytes cannot be treated as one stable file image.
    if (after != output.observation) {
        return file_snapshot_result::changed_during_read;
    }

    return file_snapshot_result::acquired;
}

} // namespace cw::server::core
