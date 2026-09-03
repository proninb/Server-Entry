#include "../server_entry/project/source/source_manager.hpp"
#include "../server_entry/project/source/source_manager_persistence.hpp"
#include "../server_entry/diagnostics/diagnostic_buffer.hpp"
#include "../server_entry/operation.hpp"
#include "../server_entry/core/hash/sha256.hpp"
#include "../server_entry/metrics/source_acquisition_telemetry.hpp"
#include "../server_entry/metrics/metrics_store.hpp"

#include <chrono>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
using namespace cw::server;
source_acquisition_telemetry test_metrics;

struct temporary_directory
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("server-entry-sm2-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    temporary_directory() { std::filesystem::create_directories(path); }
    ~temporary_directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

bool write(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::uint32_t read_le32(const std::string& bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index)
        value |= std::uint32_t(static_cast<unsigned char>(bytes[offset + index])) << (index * 8);
    return value;
}

std::uint64_t read_le64(const std::string& bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index)
        value |= std::uint64_t(static_cast<unsigned char>(bytes[offset + index])) << (index * 8);
    return value;
}

void write_le32(std::string& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned index = 0; index != 4; ++index)
        bytes[offset + index] = static_cast<char>((value >> (index * 8)) & 0xff);
}

void write_le64(std::string& bytes, std::size_t offset, std::uint64_t value)
{
    for (unsigned index = 0; index != 8; ++index)
        bytes[offset + index] = static_cast<char>((value >> (index * 8)) & 0xff);
}

void refresh_artifact_integrity(std::string& bytes)
{
    const auto digest = core::sha256(
        std::string_view{bytes}.substr(128));
    std::memcpy(bytes.data() + 72, digest.data(), digest.size());
    write_le32(bytes, 104, 0);
    const auto crc = source_manager_crc32c({
        reinterpret_cast<const std::byte*>(bytes.data()), 128});
    write_le32(bytes, 104, crc);
}

bool has_change(const source_manager_update& update, source_id id, source_change_kind kind)
{
    for (const auto& change : update.changes())
        if (change.source == id && change.kind == kind) return true;
    return false;
}

std::uint64_t counter(const metrics_store& metrics, metric_id id)
{
    return metrics.snapshot().counter(id).value;
}

bool test_hash_vectors()
{
    constexpr std::array<std::byte, 32> empty{
        std::byte{0xe3},std::byte{0xb0},std::byte{0xc4},std::byte{0x42},std::byte{0x98},std::byte{0xfc},std::byte{0x1c},std::byte{0x14},
        std::byte{0x9a},std::byte{0xfb},std::byte{0xf4},std::byte{0xc8},std::byte{0x99},std::byte{0x6f},std::byte{0xb9},std::byte{0x24},
        std::byte{0x27},std::byte{0xae},std::byte{0x41},std::byte{0xe4},std::byte{0x64},std::byte{0x9b},std::byte{0x93},std::byte{0x4c},
        std::byte{0xa4},std::byte{0x95},std::byte{0x99},std::byte{0x1b},std::byte{0x78},std::byte{0x52},std::byte{0xb8},std::byte{0x55}};
    constexpr std::array<std::byte, 32> abc{
        std::byte{0xba},std::byte{0x78},std::byte{0x16},std::byte{0xbf},std::byte{0x8f},std::byte{0x01},std::byte{0xcf},std::byte{0xea},
        std::byte{0x41},std::byte{0x41},std::byte{0x40},std::byte{0xde},std::byte{0x5d},std::byte{0xae},std::byte{0x22},std::byte{0x23},
        std::byte{0xb0},std::byte{0x03},std::byte{0x61},std::byte{0xa3},std::byte{0x96},std::byte{0x17},std::byte{0x7a},std::byte{0x9c},
        std::byte{0xb4},std::byte{0x10},std::byte{0xff},std::byte{0x61},std::byte{0xf2},std::byte{0x00},std::byte{0x15},std::byte{0xad}};
    return hash_source_content({}).bytes == empty && hash_source_content("abc").bytes == abc;
}

bool test_present_empty_and_missing()
{
    temporary_directory directory;
    const auto full = (directory.path / "full.cpp").lexically_normal();
    const auto empty = (directory.path / "empty.cpp").lexically_normal();
    const auto missing = (directory.path / "missing.cpp").lexically_normal();
    if (!write(full, "physical bytes") || !write(empty, {})) return false;
    source_manager manager;
    auto update = manager.begin_update();
    if (!update.add(full, project_item_role::source).ok() ||
        !update.add(empty, project_item_role::source).ok() ||
        !update.acquire(source_id{1}, test_metrics).ok() || !update.acquire(source_id{2}, test_metrics).ok()) return false;
    source_view candidate;
    if (!update.get_view(source_id{1}, candidate).ok() || candidate.bytes != "physical bytes" ||
        !has_change(update, source_id{1}, source_change_kind::added)) return false;
    if (!update.commit().ok()) return false;
    source_view committed;
    source_physical_state empty_state;
    if (!manager.get_view(source_id{1}, committed).ok() || committed.bytes != "physical bytes" ||
        !manager.get_view(source_id{2}, committed).ok() || !committed.bytes.empty() ||
        !manager.get_physical_state(source_id{2}, empty_state).ok() ||
        empty_state.presence != source_presence::present ||
        empty_state.hash != hash_source_content({})) return false;
    auto failed = manager.begin_update();
    if (!failed.add(missing, project_item_role::source).ok() ||
        failed.acquire(source_id{3}, test_metrics).code != status_code::configuration_failed ||
        failed.commit().ok()) return false;
    return manager.sources().size() == 2 && manager.find(source_id{3}) == nullptr;
}

bool test_candidate_lifetime_modify_and_discard()
{
    temporary_directory directory;
    const auto path = (directory.path / "a.cpp").lexically_normal();
    if (!write(path, "old")) return false;
    source_manager manager;
    { auto update = manager.begin_update(); if (!update.add(path, project_item_role::source).ok() ||
      !update.acquire(source_id{1}, test_metrics).ok() || !update.commit().ok()) return false; }
    source_physical_state before;
    if (!manager.get_physical_state(source_id{1}, before).ok() || !write(path, "new-content")) return false;
    {
        auto update = manager.begin_update();
        if (!update.add(path, project_item_role::source).ok() || !update.acquire(source_id{1}, test_metrics).ok()) return false;
        source_view candidate, committed;
        if (!update.get_view(source_id{1}, candidate).ok() || candidate.bytes != "new-content" ||
            !manager.get_view(source_id{1}, committed).ok() || committed.bytes != "old" ||
            !has_change(update, source_id{1}, source_change_kind::modified)) return false;
        const auto* stable = candidate.bytes.data();
        for (int index = 0; index < 64; ++index)
        {
            const auto extra = (directory.path / ("extra-" + std::to_string(index))).lexically_normal();
            if (!write(extra, "x") || !update.add(extra, project_item_role::source).ok() ||
                !update.acquire(source_id{static_cast<std::uint32_t>(index + 2)}, test_metrics).ok()) return false;
        }
        if (!update.get_view(source_id{1}, candidate).ok() || candidate.bytes.data() != stable ||
            candidate.bytes != "new-content") return false;
    }
    source_view committed;
    source_physical_state after;
    return manager.get_view(source_id{1}, committed).ok() && committed.bytes == "old" &&
           manager.get_physical_state(source_id{1}, after).ok() && after.observation == before.observation &&
           after.hash == before.hash && manager.sources().size() == 1;
}

bool test_commit_touch_overlay_remove_and_reappear()
{
    temporary_directory directory;
    const auto a = (directory.path / "a.cpp").lexically_normal();
    const auto b = (directory.path / "b.cpp").lexically_normal();
    if (!write(a, "old-a") || !write(b, "stable-b")) return false;
    source_manager manager;
    { auto update = manager.begin_update(); if (!update.add(a, project_item_role::source).ok() ||
      !update.add(b, project_item_role::source).ok() || !update.acquire(source_id{1}, test_metrics).ok() ||
      !update.acquire(source_id{2}, test_metrics).ok() || !update.commit().ok()) return false; }
    source_view old_a, old_b;
    if (!manager.get_view(source_id{1}, old_a).ok() || !manager.get_view(source_id{2}, old_b).ok()) return false;
    const auto* b_storage = old_b.bytes.data();
    if (!write(a, "new-a-content")) return false;
    const char* candidate_a_storage = nullptr;
    { auto update = manager.begin_update(); if (!update.add(a, project_item_role::source).ok() ||
      !update.add(b, project_item_role::source).ok() || !update.acquire(source_id{1}, test_metrics).ok() ||
      !update.acquire(source_id{2}, test_metrics).ok()) return false;
      source_view candidate_a, candidate_b;
      if (!update.get_view(source_id{1}, candidate_a).ok() || candidate_a.bytes != "new-a-content" ||
          !update.get_view(source_id{2}, candidate_b).ok() || candidate_b.bytes.data() != b_storage) return false;
      candidate_a_storage = candidate_a.bytes.data();
      if (!update.commit().ok()) return false; }
    source_view committed;
    if (!manager.get_view(source_id{1}, committed).ok() || committed.bytes != "new-a-content" ||
        committed.bytes.data() != candidate_a_storage) return false;

    source_physical_state before_touch;
    if (!manager.get_physical_state(source_id{1}, before_touch).ok()) return false;
    std::error_code error;
    const auto current_write_time=std::filesystem::last_write_time(a,error);
    if(error)return false;
    std::filesystem::last_write_time(a,current_write_time+std::chrono::seconds(2),error);
    if (error) return false;
    const auto* content_storage = committed.bytes.data();
    { auto update = manager.begin_update(); if (!update.add(a, project_item_role::source).ok() ||
      !update.acquire(source_id{1}, test_metrics).ok() || !update.changes().empty()) return false;
      source_view candidate; source_physical_state touched;
      if (!update.get_view(source_id{1}, candidate).ok() || candidate.bytes.data() != content_storage ||
          !update.get_physical_state(source_id{1}, touched).ok() || touched.hash != before_touch.hash ||
          touched.observation == before_touch.observation || !update.commit().ok()) return false; }
    if (!manager.get_view(source_id{1}, committed).ok() || committed.bytes.data() != content_storage) return false;

    std::filesystem::remove(a, error); if (error) return false;
    { auto update = manager.begin_update(); if (!update.add(a, project_item_role::source).ok() ||
      !update.acquire(source_id{1}, test_metrics).ok() || !has_change(update, source_id{1}, source_change_kind::removed)) return false;
      source_view missing; if (update.get_view(source_id{1}, missing).ok() || !update.commit().ok()) return false; }
    source_physical_state missing_state;
    if (!manager.get_physical_state(source_id{1}, missing_state).ok() ||
        missing_state.presence != source_presence::missing || manager.get_view(source_id{1}, committed).ok()) return false;
    if (!write(a, "returned")) return false;
    { auto update = manager.begin_update(); if (!update.add(a, project_item_role::source).ok() ||
      !update.acquire(source_id{1}, test_metrics).ok() || !has_change(update, source_id{1}, source_change_kind::added) ||
      !update.commit().ok()) return false; }
    return manager.sources().size() == 2 && manager.roots()[0].source == source_id{1} &&
           manager.get_view(source_id{1}, committed).ok() && committed.bytes == "returned";
}

bool test_stale_physical_candidate()
{
    temporary_directory directory;
    const auto path = (directory.path / "a.cpp").lexically_normal();
    if (!write(path, "one")) return false;
    source_manager manager;
    { auto seed = manager.begin_update(); if (!seed.add(path, project_item_role::source).ok() ||
      !seed.acquire(source_id{1}, test_metrics).ok() || !seed.commit().ok()) return false; }
    auto first = manager.begin_update();
    auto stale = manager.begin_update();
    if (!write(path, "two-two") || !first.add(path, project_item_role::source).ok() ||
        !stale.add(path, project_item_role::source).ok() || !first.acquire(source_id{1}, test_metrics).ok() ||
        !stale.acquire(source_id{1}, test_metrics).ok() || !first.commit().ok() ||
        stale.commit().code != status_code::invalid_state) return false;
    source_view committed;
    return manager.get_view(source_id{1}, committed).ok() && committed.bytes == "two-two";
}

bool test_repeated_acquire_reports_net_change()
{
    temporary_directory directory;
    const auto path = (directory.path / "net.cpp").lexically_normal();
    if (!write(path, "A")) return false;
    source_manager manager;
    { auto seed = manager.begin_update(); if (!seed.add(path, project_item_role::source).ok() ||
      !seed.acquire(source_id{1}, test_metrics).ok() || !seed.commit().ok()) return false; }

    { metrics_store metrics; source_acquisition_telemetry acquisition; auto update = manager.begin_update();
      if (!update.add(path, project_item_role::source).ok() ||
      !write(path, "BBBB") || !update.acquire(source_id{1}, acquisition).ok() ||
      !has_change(update, source_id{1}, source_change_kind::modified) ||
      !write(path, "A") || !update.acquire(source_id{1}, acquisition).ok() || !update.changes().empty()) return false;
      acquisition.flush_to(metrics);
      update.record_candidate_metrics(metrics);
      source_view candidate; if (!update.get_view(source_id{1}, candidate).ok() || candidate.bytes != "A" ||
      counter(metrics, metric_id::source_candidate_modified_count) != 0 ||
      counter(metrics, metric_id::source_candidate_added_count) != 0 ||
      counter(metrics, metric_id::source_candidate_removed_count) != 0) return false; }

    std::error_code error;
    { metrics_store metrics; source_acquisition_telemetry acquisition; auto update = manager.begin_update();
      if (!update.add(path, project_item_role::source).ok()) return false;
      std::filesystem::remove(path, error); if (error || !update.acquire(source_id{1}, acquisition).ok() ||
      !has_change(update, source_id{1}, source_change_kind::removed) || !write(path, "A") ||
      !update.acquire(source_id{1}, acquisition).ok() || !update.changes().empty()) return false;
      acquisition.flush_to(metrics);
      update.record_candidate_metrics(metrics);
      if (counter(metrics, metric_id::source_candidate_added_count) != 0 ||
          counter(metrics, metric_id::source_candidate_modified_count) != 0 ||
          counter(metrics, metric_id::source_candidate_removed_count) != 0) return false; }

    std::filesystem::remove(path, error); if (error) return false;
    { auto missing = manager.begin_update(); if (!missing.add(path, project_item_role::source).ok() ||
      !missing.acquire(source_id{1}, test_metrics).ok() || !missing.commit().ok()) return false; }
    { auto update = manager.begin_update(); if (!update.add(path, project_item_role::source).ok() ||
      !write(path, "A") || !update.acquire(source_id{1}, test_metrics).ok() ||
      !has_change(update, source_id{1}, source_change_kind::added)) return false;
      std::filesystem::remove(path, error); if (error || !update.acquire(source_id{1}, test_metrics).ok() ||
      !update.changes().empty()) return false;
      source_physical_state state; return update.get_physical_state(source_id{1}, state).ok() &&
          state.presence == source_presence::missing; }
}

bool test_rejects_observation_change_during_read()
{
    temporary_directory directory;
    const auto path = (directory.path / "moving.cpp").lexically_normal();
    const std::string content(32u * 1024u * 1024u, 'x');
    if (!write(path, content)) return false;
    source_manager manager;
    auto update = manager.begin_update();
    if (!update.add(path, project_item_role::source).ok()) return false;
    std::error_code error;
    const auto initial = std::filesystem::last_write_time(path, error);
    if (error) return false;
    std::atomic<bool> stop = false;
    std::atomic<bool> started = false;
    std::thread mutator([&]
    {
        std::uint32_t tick = 1;
        started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire))
        {
            std::error_code ignored;
            std::filesystem::last_write_time(path, initial + std::chrono::seconds(tick++), ignored);
        }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    source_acquisition_telemetry acquisition;
    metrics_store metrics;
    const auto result = update.acquire(source_id{1}, acquisition);
    stop.store(true, std::memory_order_release);
    mutator.join();
    acquisition.flush_to(metrics);
    return result.code == status_code::configuration_failed && !update.commit().ok() &&
           manager.sources().empty() &&
           counter(metrics, metric_id::source_acquisition_count) == 1 &&
           counter(metrics, metric_id::source_acquisition_failure_count) == 1 &&
           counter(metrics, metric_id::source_toctou_rejection_count) == 1;
}

bool test_acquisition_and_candidate_metrics()
{
    temporary_directory directory;
    const auto path = (directory.path / "telemetry.cpp").lexically_normal();
    if (!write(path, {})) return false;
    source_manager manager;
    source_acquisition_telemetry acquisition{metrics_mode::detailed};

    metrics_store added_metrics;
    added_metrics.set_mode(metrics_mode::detailed);
    auto added = manager.begin_update();
    reset_source_telemetry_test_clock();
    if (!added.add(path, project_item_role::source).ok() ||
        !added.acquire(source_id{1}, acquisition).ok() ||
        source_telemetry_test_clock_read_count() != 8) return false;
    acquisition.flush_to(added_metrics);
    added.record_candidate_metrics(added_metrics);
    auto snapshot = added_metrics.snapshot();
    if (snapshot.counter(metric_id::source_acquisition_count).value != 1 ||
        snapshot.counter(metric_id::source_content_read_count).value != 1 ||
        snapshot.counter(metric_id::source_bytes_read).value != 0 ||
        snapshot.counter(metric_id::source_candidate_added_count).value != 1 ||
        snapshot.duration(metric_id::source_acquisition_duration).count != 1 ||
        snapshot.duration(metric_id::source_file_open_duration).count != 1 ||
        snapshot.duration(metric_id::source_observe_before_duration).count != 1 ||
        snapshot.duration(metric_id::source_read_duration).count != 1 ||
        snapshot.duration(metric_id::source_observe_after_duration).count != 1 ||
        snapshot.duration(metric_id::source_sha256_duration).count != 1 ||
        snapshot.duration(metric_id::source_candidate_update_duration).count != 1 ||
        !added.commit().ok()) return false;
    const auto phase_total =
        snapshot.duration(metric_id::source_file_open_duration).total_ns +
        snapshot.duration(metric_id::source_observe_before_duration).total_ns +
        snapshot.duration(metric_id::source_read_duration).total_ns +
        snapshot.duration(metric_id::source_observe_after_duration).total_ns +
        snapshot.duration(metric_id::source_sha256_duration).total_ns +
        snapshot.duration(metric_id::source_candidate_update_duration).total_ns;
    if (phase_total > snapshot.duration(metric_id::source_acquisition_duration).total_ns)
        return false;

    metrics_store unchanged_metrics;
    unchanged_metrics.set_mode(metrics_mode::detailed);
    auto unchanged = manager.begin_update();
    reset_source_telemetry_test_clock();
    if (!unchanged.add(path, project_item_role::source).ok() ||
        !unchanged.acquire(source_id{1}, acquisition).ok() ||
        source_telemetry_test_clock_read_count() != 4) return false;
    acquisition.flush_to(unchanged_metrics);
    unchanged.record_candidate_metrics(unchanged_metrics);
    snapshot = unchanged_metrics.snapshot();
    if (snapshot.counter(metric_id::source_unchanged_fast_path_count).value != 1 ||
        snapshot.counter(metric_id::source_content_read_count).value != 0 ||
        snapshot.duration(metric_id::source_read_duration).count != 0 ||
        snapshot.duration(metric_id::source_observe_after_duration).count != 0 ||
        snapshot.duration(metric_id::source_sha256_duration).count != 0 ||
        snapshot.duration(metric_id::source_candidate_update_duration).count != 0 ||
        !unchanged.commit().ok()) return false;

    if (!write(path, "modified")) return false;
    metrics_store modified_metrics;
    modified_metrics.set_mode(metrics_mode::detailed);
    auto modified = manager.begin_update();
    if (!modified.add(path, project_item_role::source).ok() ||
        !modified.acquire(source_id{1}, acquisition).ok()) return false;
    acquisition.flush_to(modified_metrics);
    modified.record_candidate_metrics(modified_metrics);
    if (counter(modified_metrics, metric_id::source_bytes_read) != 8 ||
        counter(modified_metrics, metric_id::source_candidate_modified_count) != 1 ||
        !modified.commit().ok()) return false;

    source_physical_state before_touch;
    if (!manager.get_physical_state(source_id{1}, before_touch).ok()) return false;
    std::error_code error;
    const auto current_write_time=std::filesystem::last_write_time(path,error);
    if(error)return false;
    std::filesystem::last_write_time(
        path,current_write_time+std::chrono::seconds(2),error);
    if (error) return false;
    metrics_store observation_metrics;
    observation_metrics.set_mode(metrics_mode::detailed);
    auto observation = manager.begin_update();
    if (!observation.add(path, project_item_role::source).ok() ||
        !observation.acquire(source_id{1}, acquisition).ok()) return false;
    acquisition.flush_to(observation_metrics);
    observation.record_candidate_metrics(observation_metrics);
    if (counter(observation_metrics,
                metric_id::source_candidate_observation_only_count) != 1 ||
        counter(observation_metrics, metric_id::source_candidate_modified_count) != 0 ||
        !observation.commit().ok()) return false;

    std::filesystem::remove(path, error);
    if (error) return false;
    metrics_store removed_metrics;
    removed_metrics.set_mode(metrics_mode::detailed);
    auto removed = manager.begin_update();
    if (!removed.add(path, project_item_role::source).ok() ||
        !removed.acquire(source_id{1}, acquisition).ok()) return false;
    acquisition.flush_to(removed_metrics);
    removed.record_candidate_metrics(removed_metrics);
    return counter(removed_metrics, metric_id::source_candidate_removed_count) == 1 &&
           counter(removed_metrics, metric_id::source_acquisition_failure_count) == 0;
}

bool test_off_and_basic_modes()
{
    temporary_directory directory;
    const auto off_path = (directory.path / "off.cpp").lexically_normal();
    const auto basic_path = (directory.path / "basic.cpp").lexically_normal();
    if (!write(off_path, "off") || !write(basic_path, "basic")) return false;

    source_manager off_manager;
    metrics_store off_metrics;
    off_metrics.set_mode(metrics_mode::off);
    source_acquisition_telemetry off{off_metrics.mode()};
    auto off_update = off_manager.begin_update();
    reset_source_telemetry_test_clock();
    if (!off_update.add(off_path, project_item_role::source).ok() ||
        !off_update.acquire(source_id{1}, off).ok() ||
        source_telemetry_test_clock_read_count() != 0) return false;
    off.flush_to(off_metrics);
    off_update.record_candidate_metrics(off_metrics);
    const auto off_snapshot = off_metrics.snapshot();
    if (off_snapshot.counter(metric_id::source_acquisition_count).value != 0 ||
        off_snapshot.counter(metric_id::source_candidate_added_count).value != 0 ||
        off_snapshot.duration(metric_id::source_acquisition_duration).count != 0 ||
        !off_update.commit().ok()) return false;

    source_manager basic_manager;
    metrics_store basic_metrics;
    source_acquisition_telemetry basic{basic_metrics.mode()};
    auto basic_update = basic_manager.begin_update();
    reset_source_telemetry_test_clock();
    if (!basic_update.add(basic_path, project_item_role::source).ok() ||
        !basic_update.acquire(source_id{1}, basic).ok() ||
        source_telemetry_test_clock_read_count() != 0) return false;
    basic.flush_to(basic_metrics);
    basic_update.record_candidate_metrics(basic_metrics);
    const auto basic_snapshot = basic_metrics.snapshot();
    return basic_snapshot.counter(metric_id::source_acquisition_count).value == 1 &&
           basic_snapshot.counter(metric_id::source_content_read_count).value == 1 &&
           basic_snapshot.counter(metric_id::source_bytes_read).value == 5 &&
           basic_snapshot.counter(metric_id::source_candidate_added_count).value == 1 &&
           basic_snapshot.duration(metric_id::source_acquisition_duration).count == 0 &&
           basic_snapshot.duration(metric_id::source_file_open_duration).count == 0 &&
           basic_snapshot.duration(metric_id::source_observe_before_duration).count == 0 &&
           basic_snapshot.duration(metric_id::source_read_duration).count == 0 &&
           basic_snapshot.duration(metric_id::source_observe_after_duration).count == 0 &&
           basic_snapshot.duration(metric_id::source_sha256_duration).count == 0 &&
           basic_snapshot.duration(metric_id::source_candidate_update_duration).count == 0;
}

bool test_persistence_round_trip_and_vectors()
{
    if (source_path_xxh64({}) != 0xef46db3751d8e999ull ||
        source_path_fingerprint(source_path_xxh64({})) != 0xbe9e32aeu)
        return false;
    const std::string crc_text = "123456789";
    if (source_manager_crc32c({reinterpret_cast<const std::byte*>(crc_text.data()),
                               crc_text.size()}) != 0xe3069283u) return false;
    const std::pair<std::wstring, std::string> vectors[] = {
        {L"A", std::string{"\x41", 1}},
        {std::wstring{wchar_t{0x00e9}}, std::string{"\xc3\xa9", 2}},
        {std::wstring{wchar_t{0xd83d}, wchar_t{0xde00}}, std::string{"\xf0\x9f\x98\x80", 4}},
        {std::wstring{wchar_t{0xd800}}, std::string{"\xed\xa0\x80", 3}},
        {std::wstring{wchar_t{0xdc00}}, std::string{"\xed\xb0\x80", 3}}};
    for (const auto& [wide, expected] : vectors)
    {
        std::string encoded;
        std::wstring decoded;
        if (!encode_wtf8(wide, encoded).ok() || encoded != expected ||
            !decode_wtf8(encoded, decoded).ok() || decoded != wide) return false;
    }
    std::wstring rejected;
    if (decode_wtf8(std::string{"\xed\xa0\xbd\xed\xb8\x80", 6}, rejected).ok())
        return false;

    temporary_directory directory;
    const auto a = (directory.path / "A.noc").lexically_normal();
    const auto d = (directory.path / "D.noc").lexically_normal();
    const auto common = (directory.path / "common.noc").lexically_normal();
    const auto orphan = (directory.path / "orphan.noc").lexically_normal();
    const auto checkpoint = directory.path / "source_manager.bin";
    if (!write(a, "a") || !write(d, "d") || !write(common, "common") ||
        !write(orphan, "orphan")) return false;
    source_manager manager;
    source_acquisition_telemetry telemetry;
    source_id aid, did, common_id, orphan_id;
    {
        auto update = manager.begin_update();
        if (!update.resolve(a, project_item_role::source, aid).ok() ||
            !update.resolve(d, project_item_role::source, did).ok() ||
            !update.resolve_include(common, common_id).ok() ||
            !update.resolve(orphan, project_item_role::source, orphan_id).ok()) return false;
        const source_id shared[]{common_id};
        if (!update.set_includes(aid, shared).ok() || !update.set_includes(did, shared).ok() ||
            !update.set_includes(orphan_id, shared).ok()) return false;
        for (std::uint32_t id = 1; id <= 4; ++id)
            if (!update.acquire(source_id{id}, telemetry).ok()) return false;
        diagnostic_buffer diagnostics;
        if (!update.validate_source_graph(operation_id{99}, diagnostics).ok() ||
            !update.commit().ok()) return false;
    }
    {
        auto update = manager.begin_update();
        if (!update.add(a, project_item_role::source).ok() ||
            !update.add(d, project_item_role::source).ok()) return false;
        diagnostic_buffer diagnostics;
        if (!update.validate_source_graph(operation_id{100}, diagnostics).ok() ||
            !update.commit().ok()) return false;
    }
    if (!manager.save_checkpoint(checkpoint).ok() ||
        !source_manager::strict_validate_checkpoint(checkpoint).ok()) return false;
    source_manager loaded;
    if (!loaded.load_checkpoint(checkpoint).ok() || loaded.source_count() != 4 ||
        loaded.root_count() != 2) return false;
    source_id found;
    if (!loaded.find_by_path(common, found).ok() || found != common_id ||
        !loaded.find_by_path(orphan, found).ok() || found != orphan_id) return false;
    std::vector<source_id> dependencies;
    if (!loaded.get_dependencies(aid, false, dependencies).ok() ||
        dependencies != std::vector<source_id>{common_id} ||
        !loaded.get_dependencies(common_id, true, dependencies).ok() ||
        dependencies.size() != 3) return false;
    source_physical_state before, after;
    if (!manager.get_physical_state(common_id, before).ok() ||
        !loaded.get_physical_state(common_id, after).ok() ||
        before.presence != after.presence || before.observation != after.observation ||
        before.hash != after.hash) return false;
    source_view unavailable;
    if (loaded.get_view(common_id, unavailable).code != status_code::not_available) return false;

    std::ifstream input(checkpoint, std::ios::binary);
    const std::string artifact{std::istreambuf_iterator<char>{input}, {}};
    if (artifact.size() < 416) return false;
    const auto read_wire_u64=[&](std::size_t offset)
    {
        std::uint64_t value=0;
        for(unsigned byte=0;byte!=8;++byte)
            value|=static_cast<std::uint64_t>(
                static_cast<unsigned char>(artifact[offset+byte]))<<(byte*8);
        return value;
    };
    source_physical_state wire_state;
    const auto physical_offset=read_wire_u64(128+32+8);
    if(!manager.get_physical_state(common_id,wire_state).ok()||
       physical_offset+std::uint64_t(common_id.value()-1)*56+8>artifact.size()||
       read_wire_u64(static_cast<std::size_t>(physical_offset)+
                     std::size_t(common_id.value()-1)*56)!=
           wire_state.observation.write_time_ticks)return false;
    const auto corrupt = [&](std::string bytes, std::string_view name)
    {
        const auto path = directory.path / std::string{name};
        return write(path, bytes) ? path : std::filesystem::path{};
    };
    {
        auto bytes = artifact;
        bytes[0] ^= 1;
        source_manager invalid;
        if (invalid.load_checkpoint(corrupt(std::move(bytes), "bad-header.bin")).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        write_le64(bytes, 128 + 8, std::numeric_limits<std::uint64_t>::max());
        source_manager invalid;
        if (invalid.load_checkpoint(corrupt(std::move(bytes), "bad-section.bin")).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto source_core_offset = read_le64(bytes, 128 + 8);
        write_le32(bytes, static_cast<std::size_t>(source_core_offset), 0xfffffff0u);
        source_manager invalid;
        if (!invalid.load_checkpoint(corrupt(std::move(bytes), "bad-path.bin")).ok()) return false;
        std::filesystem::path value;
        if (invalid.get_path(source_id{1}, value).code != status_code::artifact_corrupt)
            return false;
    }
    {
        auto bytes = artifact;
        const auto forward_offsets = read_le64(bytes, 128 + 2 * 32 + 8);
        write_le32(bytes, static_cast<std::size_t>(forward_offsets + 4), 0xffffffffu);
        source_manager invalid;
        if (!invalid.load_checkpoint(corrupt(std::move(bytes), "bad-csr.bin")).ok()) return false;
        if (invalid.get_dependencies(source_id{1}, false, dependencies).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto roots_offset = read_le64(bytes, 128 + 6 * 32 + 8);
        write_le32(bytes, static_cast<std::size_t>(roots_offset), 0xffffffffu);
        source_manager invalid;
        if (!invalid.load_checkpoint(corrupt(std::move(bytes), "bad-root.bin")).ok()) return false;
        source_root root;
        if (invalid.get_root(0, root).code != status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto forward_edges = read_le64(bytes, 128 + 3 * 32 + 8);
        write_le32(bytes, static_cast<std::size_t>(forward_edges), 0xffffffffu);
        refresh_artifact_integrity(bytes);
        const auto path = corrupt(std::move(bytes), "bad-edge.bin");
        if (source_manager::strict_validate_checkpoint(path).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto roots_offset = read_le64(bytes, 128 + 6 * 32 + 8);
        bytes[static_cast<std::size_t>(roots_offset + 4)] = char{2};
        refresh_artifact_integrity(bytes);
        const auto path = corrupt(std::move(bytes), "bad-role.bin");
        if (source_manager::strict_validate_checkpoint(path).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto index_offset = read_le64(bytes, 128 + 7 * 32 + 8);
        const auto capacity = read_le32(bytes, 48);
        bool changed = false;
        for (std::uint32_t index = 0; index != capacity; ++index)
        {
            const auto bucket = static_cast<std::size_t>(index_offset + std::uint64_t(index) * 8);
            if (read_le32(bytes, bucket + 4) != 0)
            {
                write_le32(bytes, bucket + 4, 0xffffffffu);
                changed = true;
                break;
            }
        }
        if (!changed) return false;
        refresh_artifact_integrity(bytes);
        const auto path = corrupt(std::move(bytes), "bad-index-id.bin");
        if (source_manager::strict_validate_checkpoint(path).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto index_offset = read_le64(bytes, 128 + 7 * 32 + 8);
        const auto capacity = read_le32(bytes, 48);
        for (std::uint32_t index = 0; index != capacity; ++index)
        {
            const auto bucket = static_cast<std::size_t>(index_offset + std::uint64_t(index) * 8);
            write_le32(bytes, bucket, 1);
            write_le32(bytes, bucket + 4, 1);
        }
        source_manager invalid;
        if (!invalid.load_checkpoint(corrupt(std::move(bytes), "full-index.bin")).ok())
            return false;
        source_id ignored;
        if (invalid.find_by_path(directory.path / "not-present.noc", ignored).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto path_bytes = read_le64(bytes, 128 + 8 * 32 + 8);
        bytes[static_cast<std::size_t>(path_bytes)] = static_cast<char>(0xff);
        refresh_artifact_integrity(bytes);
        const auto path = corrupt(std::move(bytes), "bad-wtf8.bin");
        if (source_manager::strict_validate_checkpoint(path).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto reverse_edges = read_le64(bytes, 128 + 5 * 32 + 8);
        write_le32(bytes, static_cast<std::size_t>(reverse_edges), 4);
        refresh_artifact_integrity(bytes);
        const auto path = corrupt(std::move(bytes), "reverse-mismatch.bin");
        if (source_manager::strict_validate_checkpoint(path).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        const auto forward_edges = read_le64(bytes, 128 + 3 * 32 + 8);
        const auto reverse_offsets = read_le64(bytes, 128 + 4 * 32 + 8);
        const auto reverse_edges = read_le64(bytes, 128 + 5 * 32 + 8);
        write_le32(bytes, static_cast<std::size_t>(forward_edges), 2);
        write_le32(bytes, static_cast<std::size_t>(forward_edges + 4), 1);
        write_le32(bytes, static_cast<std::size_t>(forward_edges + 8), 3);
        const std::uint32_t offsets[]{0, 1, 2, 3, 3};
        const std::uint32_t edges[]{2, 1, 4};
        for (std::size_t index = 0; index != std::size(offsets); ++index)
            write_le32(bytes, static_cast<std::size_t>(reverse_offsets + index * 4),
                       offsets[index]);
        for (std::size_t index = 0; index != std::size(edges); ++index)
            write_le32(bytes, static_cast<std::size_t>(reverse_edges + index * 4),
                       edges[index]);
        refresh_artifact_integrity(bytes);
        const auto path = corrupt(std::move(bytes), "reachable-cycle.bin");
        if (source_manager::strict_validate_checkpoint(path).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        auto bytes = artifact;
        bytes.back() ^= 1;
        const auto path = corrupt(std::move(bytes), "bad-payload.bin");
        if (source_manager::strict_validate_checkpoint(path).code !=
            status_code::artifact_corrupt) return false;
    }
    {
        std::unordered_map<std::uint32_t, std::filesystem::path> fingerprints;
        std::filesystem::path first_collision;
        std::filesystem::path second_collision;
        for (std::uint32_t index = 0; index != 250'000 && first_collision.empty(); ++index)
        {
            const auto candidate = std::filesystem::path{
                L"C:\\collision-test\\source_" + std::to_wstring(index) + L".noc"};
            std::string encoded;
            if (!encode_wtf8(candidate.native(), encoded).ok()) return false;
            const auto fingerprint = source_path_fingerprint(source_path_xxh64(encoded));
            const auto [position, inserted] = fingerprints.emplace(fingerprint, candidate);
            if (!inserted && position->second != candidate)
            {
                first_collision = position->second;
                second_collision = candidate;
            }
        }
        if (first_collision.empty()) return false;
        source_manager collision_manager;
        auto update = collision_manager.begin_update();
        if (!update.add(first_collision, project_item_role::source).ok() ||
            !update.add(second_collision, project_item_role::source).ok() ||
            !update.commit().ok()) return false;
        const auto collision_file = directory.path / "collision.bin";
        if (!collision_manager.save_checkpoint(collision_file).ok()) return false;
        source_manager collision_loaded;
        source_id first_id, second_id;
        if (!collision_loaded.load_checkpoint(collision_file).ok() ||
            !collision_loaded.find_by_path(first_collision, first_id).ok() ||
            !collision_loaded.find_by_path(second_collision, second_id).ok() ||
            first_id == second_id) return false;
    }
    return true;
}
} // namespace

int main()
{
    return test_hash_vectors() && test_present_empty_and_missing() &&
           test_candidate_lifetime_modify_and_discard() &&
           test_commit_touch_overlay_remove_and_reappear() &&
           test_stale_physical_candidate() && test_repeated_acquire_reports_net_change() &&
           test_rejects_observation_change_during_read() &&
           test_acquisition_and_candidate_metrics() && test_off_and_basic_modes() &&
           test_persistence_round_trip_and_vectors() ? 0 : 1;
}
