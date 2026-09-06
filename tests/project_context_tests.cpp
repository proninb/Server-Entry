#include "../server_entry/project/project_context.hpp"

#include "../server_entry/logging/logger.hpp"
#include "../server_entry/metrics/metrics_store.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace cw::server;

class temporary_project final {
public:
    temporary_project() {
        const auto nonce =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        root =
            std::filesystem::temp_directory_path() /
            ("cw_project_context_" + std::to_string(nonce));

        std::filesystem::create_directories(root);
    }

    ~temporary_project() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    bool write(
        const std::filesystem::path& relative,
        std::string_view content) const {

        const auto path = root / relative;

        std::error_code error;
        std::filesystem::create_directories(
            path.parent_path(),
            error);

        if (error) {
            return false;
        }

        std::ofstream output{
            path,
            std::ios::binary | std::ios::trunc
        };

        output.write(
            content.data(),
            static_cast<std::streamsize>(content.size()));

        output.close();
        return !output.fail();
    }

    std::filesystem::path path(
        const std::filesystem::path& relative) const {
        return root / relative;
    }

private:
    std::filesystem::path root;
};

std::string project_document(
    std::string_view items) {

    return
        "{\"version\":1,\"name\":\"ContextTest\",\"project\":[" +
        std::string{items} +
        "],\"configuration\":{\"abi\":{\"target\":\"windows-x64\",\"pack\":8}}}";
}

bool initialize_and_load(
    temporary_project& files,
    project_context& project,
    logger& log,
    metrics_store& metrics,
    std::string_view items,
    std::string_view source_name,
    std::string_view source_text,
    std::uint64_t operation) {

    const auto configuration =
        project_document(items);

    return
        files.write("project.json", configuration) &&
        files.write(
            std::filesystem::path{source_name},
            source_text) &&
        project.initialize(
            operation_id{operation},
            log,
            metrics).ok() &&
        project.load_project(
            files.path("project.json"),
            operation_id{operation + 1},
            log,
            metrics).ok();
}

bool test_full_project_load() {
    temporary_project files;
    logger log;
    metrics_store metrics;
    project_context project;

    if (!initialize_and_load(
            files,
            project,
            log,
            metrics,
            R"({"path":"main.cpp","role":"source"})",
            "main.cpp",
            "enum Mode : int { Ready = 1 };",
            1)) {
        return false;
    }

    const runtime* execution = nullptr;

    return
        project.state() == project_state::valid &&
        project.runtime_access(execution).ok() &&
        execution != nullptr;
}

bool test_invalid_configuration_is_fail_closed() {
    temporary_project files;
    logger log;
    metrics_store metrics;
    project_context project;

    if (!project.initialize(
            operation_id{3},
            log,
            metrics).ok()) {
        return false;
    }

    const auto result = project.load_project(
        files.path("missing.json"),
        operation_id{4},
        log,
        metrics);

    const runtime* execution = nullptr;

    return
        !result.ok() &&
        project.state() == project_state::error &&
        !project.diagnostics().empty() &&
        !project.runtime_access(execution).ok() &&
        execution == nullptr;
}

bool test_parser_failure_is_fail_closed() {
    temporary_project files;
    logger log;
    metrics_store metrics;
    project_context project;

    const auto configuration =
        project_document(
            R"({"path":"main.cpp","role":"source"})");

    if (!files.write("project.json", configuration) ||
        !files.write(
            "main.cpp",
            "enum Broken : int { Value = ; };") ||
        !project.initialize(
            operation_id{5},
            log,
            metrics).ok()) {
        return false;
    }

    const auto result = project.load_project(
        files.path("project.json"),
        operation_id{6},
        log,
        metrics);

    return
        !result.ok() &&
        project.state() == project_state::error &&
        !project.diagnostics().empty();
}

bool test_no_change_is_o1_frontend() {
    temporary_project files;
    logger log;
    metrics_store metrics;
    project_context project;

    const auto configuration =
        project_document(
            R"({"path":"a.cpp","role":"source"},{"path":"b.cpp","role":"source"})");

    if (!files.write("project.json", configuration) ||
        !files.write(
            "a.cpp",
            "enum A : int { ValueA = 1 };") ||
        !files.write(
            "b.cpp",
            "enum B : int { ValueB = 2 };") ||
        !project.initialize(
            operation_id{7},
            log,
            metrics).ok() ||
        !project.load_project(
            files.path("project.json"),
            operation_id{8},
            log,
            metrics).ok()) {
        return false;
    }

    if (!project.rebuild_sources(
            operation_id{9},
            log,
            metrics).ok()) {
        return false;
    }

    const auto summary =
        project.last_frontend_summary();

    return
        summary.dirty == 0 &&
        summary.checked == 0 &&
        summary.affected == 0 &&
        summary.lex == 0 &&
        summary.parse == 0 &&
        summary.publish == 0;
}

bool test_sparse_independent_rebuild() {
    temporary_project files;

    const auto configuration =
        project_document(
            R"({"path":"a.cpp","role":"source"},{"path":"b.cpp","role":"source"})");

    if (!files.write("project.json", configuration) ||
        !files.write("a.cpp", "enum A : int { ValueA = 1 };") ||
        !files.write("b.cpp", "enum B : int { ValueB = 2 };")) {
        return false;
    }

    logger log;
    metrics_store metrics;
    project_context project;

    if (!project.initialize(
            operation_id{11},
            log,
            metrics).ok() ||
        !project.load_project(
            files.path("project.json"),
            operation_id{12},
            log,
            metrics).ok()) {
        return false;
    }

    if (!files.write(
            "a.cpp",
            "enum A : int { ValueA = 1001 };")) {
        return false;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds{10});

    if (!project.rebuild_sources(
            operation_id{13},
            log,
            metrics).ok()) {
        return false;
    }

    const auto summary =
        project.last_frontend_summary();

    return
        summary.dirty == 1 &&
        summary.checked == 1 &&
        summary.affected == 1 &&
        summary.lex == 1 &&
        summary.parse == 1 &&
        summary.publish == 1 &&
        !summary.reconciliation;
}

bool test_dependent_closure_rebuild() {
    temporary_project files;

    const auto configuration =
        project_document(
            R"({"path":"root.cpp","role":"source"})");

    if (!files.write("project.json", configuration) ||
        !files.write(
            "base.hpp",
            "enum Base : int { BaseValue = 1 };") ||
        !files.write(
            "middle.hpp",
            "#include \"base.hpp\"\n"
            "enum Middle : int { MiddleValue = BaseValue };") ||
        !files.write(
            "root.cpp",
            "#include \"middle.hpp\"\n"
            "enum Root : int { RootValue = MiddleValue };")) {
        return false;
    }

    logger log;
    metrics_store metrics;
    project_context project;

    if (!project.initialize(
            operation_id{14},
            log,
            metrics).ok() ||
        !project.load_project(
            files.path("project.json"),
            operation_id{15},
            log,
            metrics).ok()) {
        return false;
    }

    if (!files.write(
            "base.hpp",
            "enum Base : int { BaseValue = 10001 };")) {
        return false;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds{10});

    if (!project.rebuild_sources(
            operation_id{16},
            log,
            metrics).ok()) {
        return false;
    }

    const auto summary =
        project.last_frontend_summary();

    return
        summary.dirty == 1 &&
        summary.checked == 1 &&
        summary.affected == 3 &&
        summary.lex == 3 &&
        summary.parse == 3 &&
        summary.publish == 3;
}

void print_recovery_diagnostic(
    std::string_view phase,
    int attempt,
    status result,
    const project_context& project) {

    const auto summary =
        project.last_frontend_summary();

    const runtime* execution = nullptr;
    const auto runtime_result =
        project.runtime_access(execution);

    std::cerr
        << "RECOVERY_DIAG"
        << " phase=" << phase
        << " attempt=" << attempt
        << " status=" << static_cast<std::uint32_t>(result.code)
        << " state=" << static_cast<std::uint32_t>(project.state())
        << " diagnostics=" << project.diagnostics().records().size()
        << " runtime_status="
        << static_cast<std::uint32_t>(runtime_result.code)
        << " runtime_ptr=" << (execution != nullptr ? 1 : 0)
        << " dirty=" << summary.dirty
        << " checked=" << summary.checked
        << " affected=" << summary.affected
        << " working_states=" << summary.working_states
        << " discovery=" << summary.discovery
        << " lex=" << summary.lex
        << " parse=" << summary.parse
        << " publish=" << summary.publish
        << " reconciliation="
        << (summary.reconciliation ? 1 : 0)
        << '\n';
}

bool wait_for_failed_incremental_rebuild(
    project_context& project,
    logger& log,
    metrics_store& metrics,
    std::uint64_t operation) {

    constexpr auto attempts = 200;
    constexpr auto retry_delay =
        std::chrono::milliseconds{5};

    status last_result{};

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        last_result = project.rebuild_sources(
            operation_id{operation},
            log,
            metrics);

        if (!last_result.ok()) {
            const auto valid_failure =
                project.state() == project_state::error &&
                !project.diagnostics().empty();

            if (!valid_failure) {
                print_recovery_diagnostic(
                    "bad_write_invalid_failure_state",
                    attempt,
                    last_result,
                    project);
            }

            return valid_failure;
        }

        std::this_thread::sleep_for(retry_delay);
    }

    print_recovery_diagnostic(
        "bad_write_timeout",
        attempts,
        last_result,
        project);

    return false;
}

bool wait_for_recovered_incremental_rebuild(
    project_context& project,
    logger& log,
    metrics_store& metrics,
    std::uint64_t operation) {

    constexpr auto attempts = 200;
    constexpr auto retry_delay =
        std::chrono::milliseconds{5};

    status last_result{};

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        last_result = project.rebuild_sources(
            operation_id{operation},
            log,
            metrics);

        if (last_result.ok() &&
            project.state() == project_state::valid) {
            const runtime* execution = nullptr;
            const auto runtime_result =
                project.runtime_access(execution);

            if (runtime_result.ok() &&
                execution != nullptr) {
                return true;
            }

            print_recovery_diagnostic(
                "recovery_runtime_invalid",
                attempt,
                runtime_result,
                project);

            return false;
        }

        std::this_thread::sleep_for(retry_delay);
    }

    print_recovery_diagnostic(
        "recovery_timeout",
        attempts,
        last_result,
        project);

    return false;
}

bool test_failed_incremental_recovers() {
    temporary_project files;
    logger log;
    metrics_store metrics;
    project_context project;

    if (!initialize_and_load(
            files,
            project,
            log,
            metrics,
            R"({"path":"a.cpp","role":"source"})",
            "a.cpp",
            "enum A : int { ValueA = 1 };",
            17)) {
        return false;
    }

    if (!files.write(
            "a.cpp",
            "enum A : int { ValueA = ; };")) {
        return false;
    }

    if (!wait_for_failed_incremental_rebuild(
            project,
            log,
            metrics,
            19)) {
        return false;
    }

    if (!files.write(
            "a.cpp",
            "enum A : int { ValueA = 2 };")) {
        return false;
    }

    return wait_for_recovered_incremental_rebuild(
        project,
        log,
        metrics,
        20);
}

} // namespace

int main() {
    const struct test_case {
        const char* name;
        bool (*run)();
    } tests[] = {
        {"full Project -> Source -> Parser -> Graph -> Runtime load", test_full_project_load},
        {"invalid project configuration fail-closed", test_invalid_configuration_is_fail_closed},
        {"Parser failure fail-closed", test_parser_failure_is_fail_closed},
        {"no-change rebuild touches zero Sources", test_no_change_is_o1_frontend},
        {"dirty independent Source rebuild is O(K)", test_sparse_independent_rebuild},
        {"dirty Source dependent closure rebuild", test_dependent_closure_rebuild},
        {"failed incremental Parser generation recovers", test_failed_incremental_recovers}
    };

    for (const auto& test : tests) {
        if (!test.run()) {
            std::cerr << "FAILED: " << test.name << '\n';
            return 1;
        }

        std::cout << "PASS: " << test.name << '\n';
    }

    return 0;
}
