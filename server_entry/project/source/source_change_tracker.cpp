#include "source_change_tracker.hpp"

#include "source_manager.hpp"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <array>
#include <cwctype>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cw::server {
namespace {

std::wstring path_key(
    const std::filesystem::path& path) {

    auto key =
        path.lexically_normal().native();

    std::transform(
        key.begin(),
        key.end(),
        key.begin(),
        [](wchar_t value) noexcept {
            return static_cast<wchar_t>(
                std::towlower(value));
        });

    return key;
}

} // namespace

class source_change_tracker::implementation final {
public:
    struct directory_watch {
        std::filesystem::path directory;
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        alignas(DWORD)
            std::array<std::byte, 60 * 1024> buffer{};
    };

    ~implementation() {
        stop();
    }

    status initialize(
        const source_manager& sources,
        const std::filesystem::path& project_configuration) noexcept {

        stop();

        try {
            completion_port = CreateIoCompletionPort(
                INVALID_HANDLE_VALUE,
                nullptr,
                0,
                1);

            if (completion_port == nullptr) {
                return {status_code::initialization_failed};
            }

            pending_event = CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                nullptr);

            if (pending_event == nullptr) {
                stop();
                return {status_code::initialization_failed};
            }

            project_key =
                path_key(project_configuration);

            const auto records = sources.sources();

            for (const auto& record : records) {
                auto result = add_source(
                    record.id,
                    record.path);

                if (!result.ok()) {
                    stop();
                    return result;
                }
            }

            known_source_count = records.size();

            auto result = ensure_watch(
                project_configuration.parent_path());

            if (!result.ok()) {
                stop();
                return result;
            }

            stopping = false;
            running = true;

            worker = std::jthread(
                [this]() noexcept {
                    run();
                });

            return {};
        }
        catch (const std::bad_alloc&) {
            stop();
            return {status_code::initialization_failed};
        }
        catch (...) {
            stop();
            return {status_code::initialization_failed};
        }
    }

    status synchronize(
        const source_manager& sources) noexcept {

        if (!running) {
            return {status_code::not_available};
        }

        try {
            const auto records = sources.sources();

            if (known_source_count > records.size()) {
                require_rescan();
                return {status_code::invalid_state};
            }

            for (std::size_t index = known_source_count;
                 index < records.size();
                 ++index) {
                const auto& record = records[index];

                const auto result = add_source(
                    record.id,
                    record.path);

                if (!result.ok()) {
                    require_rescan();
                    return result;
                }
            }

            known_source_count = records.size();
            return {};
        }
        catch (...) {
            require_rescan();
            return {status_code::initialization_failed};
        }
    }

    status drain(
        source_change_batch& output,
        std::chrono::milliseconds grace) noexcept {

        output = {};

        if (!running ||
            pending_event == nullptr) {
            output.rescan_required = true;
            return {status_code::not_available};
        }

        const auto milliseconds =
            grace.count() <= 0
                ? DWORD{0}
                : static_cast<DWORD>(
                      (std::min<std::int64_t>)(
                          grace.count(),
                          1000));

        WaitForSingleObject(
            pending_event,
            milliseconds);

        try {
            std::lock_guard lock{mutex};

            output.dirty_sources.swap(
                pending_sources);

            pending_values.clear();

            output.rescan_required =
                rescan_required;

            output.project_configuration_changed =
                project_changed;

            rescan_required = false;
            project_changed = false;

            ResetEvent(pending_event);
            return {};
        }
        catch (...) {
            output = {};
            output.rescan_required = true;
            require_rescan();
            return {status_code::initialization_failed};
        }
    }

    void require_rescan() noexcept {
        std::lock_guard lock{mutex};
        rescan_required = true;

        if (pending_event != nullptr) {
            SetEvent(pending_event);
        }
    }

    bool active() const noexcept {
        return running;
    }

    void stop() noexcept {
        running = false;
        stopping = true;

        if (completion_port != nullptr) {
            for (const auto& watch : watches) {
                if (watch &&
                    watch->handle != INVALID_HANDLE_VALUE) {
                    CancelIoEx(
                        watch->handle,
                        &watch->overlapped);
                }
            }

            PostQueuedCompletionStatus(
                completion_port,
                0,
                0,
                nullptr);
        }

        if (worker.joinable()) {
            worker.join();
        }

        for (auto& watch : watches) {
            if (watch &&
                watch->handle != INVALID_HANDLE_VALUE) {
                CloseHandle(watch->handle);
                watch->handle = INVALID_HANDLE_VALUE;
            }
        }

        watches.clear();
        watch_by_directory.clear();
        source_by_path.clear();
        project_key.clear();
        known_source_count = 0;

        {
            std::lock_guard lock{mutex};
            pending_sources.clear();
            pending_values.clear();
            rescan_required = false;
            project_changed = false;
        }

        if (pending_event != nullptr) {
            CloseHandle(pending_event);
            pending_event = nullptr;
        }

        if (completion_port != nullptr) {
            CloseHandle(completion_port);
            completion_port = nullptr;
        }

        stopping = false;
    }

private:
    status add_source(
        source_id source,
        const std::filesystem::path& path) {

        if (!source ||
            path.empty()) {
            return {status_code::configuration_failed};
        }

        const auto key = path_key(path);

        {
            std::lock_guard lock{mutex};
            source_by_path.emplace(
                key,
                source);
        }

        return ensure_watch(
            path.parent_path());
    }

    status ensure_watch(
        const std::filesystem::path& directory) {

        if (directory.empty()) {
            return {status_code::configuration_failed};
        }

        const auto key =
            path_key(directory);

        if (watch_by_directory.find(key) !=
            watch_by_directory.end()) {
            return {};
        }

        auto watch =
            std::make_unique<directory_watch>();

        watch->directory =
            directory.lexically_normal();

        watch->handle = CreateFileW(
            watch->directory.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OVERLAPPED,
            nullptr);

        if (watch->handle ==
            INVALID_HANDLE_VALUE) {
            return {status_code::not_available};
        }

        if (CreateIoCompletionPort(
                watch->handle,
                completion_port,
                reinterpret_cast<ULONG_PTR>(
                    watch.get()),
                0) == nullptr) {
            CloseHandle(watch->handle);
            watch->handle = INVALID_HANDLE_VALUE;
            return {status_code::initialization_failed};
        }

        const auto result =
            arm(*watch);

        if (!result.ok()) {
            CloseHandle(watch->handle);
            watch->handle = INVALID_HANDLE_VALUE;
            return result;
        }

        auto* pointer = watch.get();

        watches.push_back(
            std::move(watch));

        watch_by_directory.emplace(
            key,
            pointer);

        return {};
    }

    status arm(
        directory_watch& watch) noexcept {

        watch.overlapped = {};

        DWORD ignored = 0;

        const auto success =
            ReadDirectoryChangesW(
                watch.handle,
                watch.buffer.data(),
                static_cast<DWORD>(
                    watch.buffer.size()),
                FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_CREATION,
                nullptr,
                &watch.overlapped,
                nullptr);

        if (!success) {
            const auto error = GetLastError();

            if (error != ERROR_IO_PENDING) {
                return {status_code::not_available};
            }
        }

        return {};
    }

    void run() noexcept {
        for (;;) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* overlapped = nullptr;

            const auto success =
                GetQueuedCompletionStatus(
                    completion_port,
                    &bytes,
                    &key,
                    &overlapped,
                    INFINITE);

            if (stopping &&
                key == 0 &&
                overlapped == nullptr) {
                break;
            }

            if (key == 0 ||
                overlapped == nullptr) {
                if (!stopping) {
                    require_rescan();
                }
                continue;
            }

            auto* watch =
                reinterpret_cast<directory_watch*>(
                    key);

            if (!success ||
                bytes == 0) {
                if (!stopping) {
                    require_rescan();

                    if (!arm(*watch).ok()) {
                        require_rescan();
                    }
                }
                continue;
            }

            consume(*watch, bytes);

            if (!stopping &&
                !arm(*watch).ok()) {
                require_rescan();
            }
        }
    }

    void consume(
        directory_watch& watch,
        DWORD bytes) noexcept {

        try {
            std::size_t offset = 0;

            while (offset < bytes) {
                const auto* entry =
                    reinterpret_cast<
                        const FILE_NOTIFY_INFORMATION*>(
                        watch.buffer.data() + offset);

                const std::wstring name{
                    entry->FileName,
                    entry->FileNameLength /
                        sizeof(wchar_t)
                };

                const auto full_path =
                    watch.directory /
                    std::filesystem::path{name};

                const auto key =
                    path_key(full_path);

                {
                    std::lock_guard lock{mutex};

                    if (!project_key.empty() &&
                        key == project_key) {
                        project_changed = true;
                        SetEvent(pending_event);
                    }

                    const auto found =
                        source_by_path.find(key);

                    if (found != source_by_path.end()) {
                        const auto value =
                            found->second.value();

                        if (pending_values.insert(
                                value).second) {
                            pending_sources.push_back(
                                found->second);
                        }

                        SetEvent(pending_event);
                    }
                }

                if (entry->NextEntryOffset == 0) {
                    break;
                }

                offset +=
                    entry->NextEntryOffset;
            }
        }
        catch (...) {
            require_rescan();
        }
    }

    HANDLE completion_port = nullptr;
    HANDLE pending_event = nullptr;

    std::jthread worker;
    std::atomic_bool running{false};
    std::atomic_bool stopping{false};

    std::vector<std::unique_ptr<directory_watch>>
        watches;

    std::unordered_map<
        std::wstring,
        directory_watch*> watch_by_directory;

    mutable std::mutex mutex;

    std::unordered_map<
        std::wstring,
        source_id> source_by_path;

    std::wstring project_key;
    std::size_t known_source_count = 0;

    std::vector<source_id> pending_sources;
    std::unordered_set<std::uint32_t> pending_values;

    bool rescan_required = false;
    bool project_changed = false;
};

source_change_tracker::source_change_tracker() = default;

source_change_tracker::~source_change_tracker() {
    stop();
}

status source_change_tracker::initialize(
    const source_manager& sources,
    const std::filesystem::path& project_configuration) noexcept {

    try {
        if (!state) {
            state = std::make_unique<implementation>();
        }

        return state->initialize(
            sources,
            project_configuration);
    }
    catch (...) {
        state.reset();
        return {status_code::initialization_failed};
    }
}

status source_change_tracker::synchronize(
    const source_manager& sources) noexcept {

    return state
        ? state->synchronize(sources)
        : status{status_code::not_available};
}

status source_change_tracker::drain(
    source_change_batch& output,
    std::chrono::milliseconds grace) noexcept {

    if (!state) {
        output = {};
        output.rescan_required = true;
        return {status_code::not_available};
    }

    return state->drain(
        output,
        grace);
}

void source_change_tracker::require_rescan() noexcept {
    if (state) {
        state->require_rescan();
    }
}

void source_change_tracker::stop() noexcept {
    if (state) {
        state->stop();
    }
}

bool source_change_tracker::active() const noexcept {
    return state && state->active();
}

} // namespace cw::server
