#include "../server_entry/project/source/source_manager.hpp"
#include "../server_entry/metrics/metrics_store.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
using namespace cw::server;
using clock_type = std::chrono::steady_clock;

double ms(clock_type::time_point begin, clock_type::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

bool run(std::uint32_t count)
{
    metrics_store metrics;
    metrics.set_mode(metrics_mode::detailed);
    source_manager manager;
    auto update = manager.begin_update();
    for (std::uint32_t index = 0; index != count; ++index)
    {
        const auto path = std::filesystem::path{L"C:\\persistence-benchmark\\common\\prefix\\source_" +
            std::to_wstring(index) + L".noc"};
        if (!update.add(path, project_item_role::source).ok()) return false;
    }
    if (!update.commit().ok()) return false;
    const auto checkpoint = std::filesystem::absolute(
        std::filesystem::path{"out"} /
        ("source-manager-" + std::to_string(count) + ".bin"));
    const auto save_begin = clock_type::now();
    if (!manager.save_checkpoint(checkpoint, &metrics).ok()) return false;
    const auto save_end = clock_type::now();
    clock_type::time_point load_begin;
    clock_type::time_point load_end;
    {
        source_manager loaded;
        load_begin = clock_type::now();
        if (!loaded.load_checkpoint(checkpoint).ok()) return false;
        load_end = clock_type::now();
    }
    const auto strict_begin = clock_type::now();
    if (!source_manager::strict_validate_checkpoint(checkpoint).ok()) return false;
    const auto strict_end = clock_type::now();
    std::error_code error;
    const auto bytes = std::filesystem::file_size(checkpoint, error);
    std::cout << "sources=" << count << " bytes=" << bytes
              << " save_ms=" << ms(save_begin, save_end)
              << " normal_load_ms=" << ms(load_begin, load_end)
              << " strict_validate_ms=" << ms(strict_begin, strict_end) << '\n';
    const auto snapshot = metrics.snapshot();
    const auto print_duration = [&](metric_id id, const char* name)
    {
        std::cout << name << "_ms="
                  << snapshot.duration(id).total_ns / 1'000'000.0 << '\n';
    };
    print_duration(metric_id::source_checkpoint_save_duration, "total");
    print_duration(metric_id::source_checkpoint_snapshot_layout_duration, "snapshot_layout");
    print_duration(metric_id::source_checkpoint_path_materialization_duration, "path_materialization");
    print_duration(metric_id::source_checkpoint_path_index_duration, "path_index");
    print_duration(metric_id::source_checkpoint_source_state_duration, "source_state");
    print_duration(metric_id::source_checkpoint_forward_csr_duration, "dependency_csr");
    print_duration(metric_id::source_checkpoint_reverse_csr_duration, "reverse_csr");
    print_duration(metric_id::source_checkpoint_payload_sha256_duration, "payload_sha256");
    print_duration(metric_id::source_checkpoint_write_duration, "write");
    print_duration(metric_id::source_checkpoint_flush_duration, "flush");
    print_duration(metric_id::source_checkpoint_reopen_map_duration, "reopen_map");
    print_duration(metric_id::source_checkpoint_artifact_validation_duration, "artifact_validation");
    print_duration(metric_id::source_checkpoint_deep_validation_duration, "deep_validation");
    std::filesystem::remove(checkpoint, error);
    return !error;
}
}

int main(int argc, char** argv)
{
    const auto count = argc == 2 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 100'000u;
    return run(count) ? 0 : 1;
}
