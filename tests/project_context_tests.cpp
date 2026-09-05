#include "../server_entry/project/project_context.hpp"

#include "../server_entry/logging/logger.hpp"
#include "../server_entry/metrics/metrics_store.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

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

    temporary_project(const temporary_project&) = delete;
    temporary_project& operator=(const temporary_project&) = delete;

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

        return output.good();
    }

    std::filesystem::path path(
        const std::filesystem::path& relative) const {

        return root / relative;
    }

private:
    std::filesystem::path root;
};

constexpr std::string_view project_json =
    R"({"version":1,"name":"ContextTest","project":[{"path":"main.cpp","role":"source"}],"configuration":{"abi":{"target":"windows-x64","pack":8}}})";

bool test_full_project_load() {
    temporary_project files;

    if (!files.write("project.json", project_json) ||
        !files.write(
            "main.cpp",
            "enum Mode : int { Ready = 1 };")) {
        return false;
    }

    logger log;
    metrics_store metrics;
    project_context project;

    if (!project.initialize(
            operation_id{1},
            log,
            metrics).ok()) {
        return false;
    }

    if (!project.load_project(
            files.path("project.json"),
            operation_id{2},
            log,
            metrics).ok()) {
        return false;
    }

    const runtime* execution = nullptr;

    return
        project.state() == project_state::valid &&
        project.diagnostics().empty() &&
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

    if (!files.write("project.json", project_json) ||
        !files.write(
            "main.cpp",
            "enum Broken : int { Value = ; };")) {
        return false;
    }

    logger log;
    metrics_store metrics;
    project_context project;

    if (!project.initialize(
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

    const runtime* execution = nullptr;

    return
        !result.ok() &&
        project.state() == project_state::error &&
        !project.diagnostics().empty() &&
        !project.runtime_access(execution).ok() &&
        execution == nullptr;
}

} // namespace

int main() {
    const struct test_case {
        const char* name;
        bool (*run)();
    } tests[] = {
        {"full Project -> Source -> Parser -> Graph -> Runtime load", test_full_project_load},
        {"invalid project configuration fail-closed", test_invalid_configuration_is_fail_closed},
        {"Parser failure fail-closed", test_parser_failure_is_fail_closed}
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
