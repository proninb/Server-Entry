#include "../server_entry/project/project_context.hpp"

#include "../server_entry/logging/logger.hpp"
#include "../server_entry/metrics/metric.hpp"
#include "../server_entry/metrics/metrics_snapshot.hpp"
#include "../server_entry/metrics/metrics_store.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace cw::server;
using clock_type = std::chrono::steady_clock;

struct temporary_workspace final {
    temporary_workspace() {
        const auto nonce =
            clock_type::now().time_since_epoch().count();

        root =
            std::filesystem::temp_directory_path() /
            ("cw_rc_v2_03_" + std::to_string(nonce));

        std::filesystem::create_directories(root / "sources");
    }

    ~temporary_workspace() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path root;
};

struct metric_delta {
    std::uint64_t acquisitions = 0;
    std::uint64_t fast_paths = 0;
    std::uint64_t content_reads = 0;
    std::uint64_t bytes_read = 0;
};

std::uint64_t counter_delta(
    const metrics_snapshot& before,
    const metrics_snapshot& after,
    metric_id id) {

    return
        after.counter(id).value -
        before.counter(id).value;
}

metric_delta source_delta(
    const metrics_snapshot& before,
    const metrics_snapshot& after) {

    return {
        counter_delta(before, after, metric_id::source_acquisition_count),
        counter_delta(before, after, metric_id::source_unchanged_fast_path_count),
        counter_delta(before, after, metric_id::source_content_read_count),
        counter_delta(before, after, metric_id::source_bytes_read)
    };
}

double elapsed_ms(
    clock_type::time_point begin,
    clock_type::time_point end) {

    return std::chrono::duration<double, std::milli>(
        end - begin).count();
}

std::string source_name(std::uint32_t index) {
    return "s" + std::to_string(index) + ".cpp";
}

std::string source_text(
    std::uint32_t index,
    std::uint32_t value,
    bool changed = false) {

    auto text =
        "enum E" + std::to_string(index) +
        " : int { V" + std::to_string(index) +
        " = " + std::to_string(value) + " };";

    if (changed) {
        text += " /* changed */";
    }

    return text;
}

bool write_text(
    const std::filesystem::path& path,
    std::string_view text) {

    std::ofstream output{
        path,
        std::ios::binary | std::ios::trunc
    };

    if (!output) {
        return false;
    }

    output.write(
        text.data(),
        static_cast<std::streamsize>(text.size()));

    output.close();
    return !output.fail();
}

bool create_sources(
    const temporary_workspace& workspace,
    std::uint32_t count) {

    for (std::uint32_t index = 0;
         index < count;
         ++index) {
        if (!write_text(
                workspace.root / "sources" / source_name(index),
                source_text(index, index))) {
            return false;
        }
    }

    return true;
}

bool write_project(
    const temporary_workspace& workspace,
    std::uint32_t count,
    std::filesystem::path& output_path) {

    const auto directory =
        workspace.root /
        ("workload_" + std::to_string(count));

    std::filesystem::create_directories(directory);
    output_path = directory / "project.json";

    std::ofstream output{
        output_path,
        std::ios::binary | std::ios::trunc
    };

    if (!output) {
        return false;
    }

    output <<
        "{\"version\":1,\"name\":\"RC-V2-03\",\"project\":[";

    for (std::uint32_t index = 0;
         index < count;
         ++index) {
        if (index != 0) {
            output << ',';
        }

        output <<
            "{\"path\":\"../sources/" <<
            source_name(index) <<
            "\",\"role\":\"source\"}";
    }

    output <<
        "],\"configuration\":{"
        "\"abi\":{\"target\":\"windows-x64\",\"pack\":8}}}";

    output.close();
    return !output.fail();
}

void print_row(
    std::uint32_t count,
    std::string_view scenario,
    double total_ms,
    const metric_delta& metrics,
    const source_frontend_summary& frontend) {

    std::cout
        << count << ','
        << scenario << ','
        << std::fixed << std::setprecision(6)
        << total_ms << ','
        << metrics.acquisitions << ','
        << metrics.fast_paths << ','
        << metrics.content_reads << ','
        << metrics.bytes_read << ','
        << frontend.dirty << ','
        << frontend.checked << ','
        << frontend.affected << ','
        << frontend.lex << ','
        << frontend.parse << ','
        << frontend.publish << ','
        << (frontend.reconciliation ? 1 : 0)
        << '\n';
}

bool gate_no_change(
    const metric_delta& metrics,
    const source_frontend_summary& frontend) {

    return
        metrics.acquisitions == 0 &&
        metrics.fast_paths == 0 &&
        metrics.content_reads == 0 &&
        metrics.bytes_read == 0 &&
        frontend.dirty == 0 &&
        frontend.checked == 0 &&
        frontend.affected == 0 &&
        frontend.lex == 0 &&
        frontend.parse == 0 &&
        frontend.publish == 0 &&
        !frontend.reconciliation;
}

bool gate_modify_one(
    const metric_delta& metrics,
    const source_frontend_summary& frontend) {

    return
        metrics.acquisitions == 1 &&
        metrics.content_reads == 1 &&
        frontend.dirty == 1 &&
        frontend.checked == 1 &&
        frontend.affected == 1 &&
        frontend.lex == 1 &&
        frontend.parse == 1 &&
        frontend.publish == 1 &&
        !frontend.reconciliation;
}

bool run_workload(
    const temporary_workspace& workspace,
    std::uint32_t count,
    std::uint64_t& operation_value) {

    std::filesystem::path project_path;

    if (!write_project(workspace, count, project_path)) {
        return false;
    }

    logger log;
    metrics_store metrics;
    project_context project;

    if (!project.initialize(
            operation_id{operation_value++},
            log,
            metrics).ok()) {
        return false;
    }

    {
        const auto before = metrics.snapshot();
        const auto begin = clock_type::now();

        const auto result = project.load_project(
            project_path,
            operation_id{operation_value++},
            log,
            metrics);

        const auto end = clock_type::now();
        const auto after = metrics.snapshot();

        if (!result.ok()) {
            return false;
        }

        print_row(
            count,
            "g0_initial",
            elapsed_ms(begin, end),
            source_delta(before, after),
            project.last_frontend_summary());
    }

    {
        const auto before = metrics.snapshot();
        const auto begin = clock_type::now();

        const auto result = project.rebuild_sources(
            operation_id{operation_value++},
            log,
            metrics);

        const auto end = clock_type::now();
        const auto after = metrics.snapshot();

        if (!result.ok()) {
            return false;
        }

        const auto delta = source_delta(before, after);
        const auto frontend = project.last_frontend_summary();

        print_row(
            count,
            "g1_no_change",
            elapsed_ms(begin, end),
            delta,
            frontend);

        if (!gate_no_change(delta, frontend)) {
            std::cerr
                << "RC-V2-03 FAIL: no-change gate "
                << count << '\n';
            return false;
        }
    }

    {
        const auto target = count - 1;

        if (!write_text(
                workspace.root / "sources" / source_name(target),
                source_text(
                    target,
                    1000000u + target,
                    true))) {
            return false;
        }

        // Notification delivery is intentionally outside measured rebuild time.
        std::this_thread::sleep_for(
            std::chrono::milliseconds{10});

        const auto before = metrics.snapshot();
        const auto begin = clock_type::now();

        const auto result = project.rebuild_sources(
            operation_id{operation_value++},
            log,
            metrics);

        const auto end = clock_type::now();
        const auto after = metrics.snapshot();

        if (!result.ok()) {
            return false;
        }

        const auto delta = source_delta(before, after);
        const auto frontend = project.last_frontend_summary();

        print_row(
            count,
            "g2_modify_one",
            elapsed_ms(begin, end),
            delta,
            frontend);

        if (!gate_modify_one(delta, frontend)) {
            std::cerr
                << "RC-V2-03 FAIL: modify-one gate "
                << count << '\n';
            return false;
        }
    }

    return true;
}

} // namespace

int main() {
    constexpr std::array<std::uint32_t, 5> workloads{
        128,
        512,
        2048,
        8192,
        32768
    };

    temporary_workspace workspace;

    if (!create_sources(
            workspace,
            workloads.back())) {
        std::cerr << "RC-V2-03 FAIL: dataset creation\n";
        return 1;
    }

    std::cout
        << "sources,scenario,total_ms,"
        << "acquisitions,fast_paths,content_reads,bytes_read,"
        << "dirty,checked,affected,lex,parse,publish,reconciliation\n";

    std::uint64_t operation_value = 1;

    for (const auto count : workloads) {
        if (!run_workload(
                workspace,
                count,
                operation_value)) {
            return 1;
        }
    }

    std::cout << "RC-V2-03 PASS\n";
    return 0;
}
