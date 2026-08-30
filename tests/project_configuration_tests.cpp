#include "../server_entry/project/project_configuration_loader.hpp"
#include "../server_entry/diagnostics/diagnostic_descriptor.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace
{
using namespace cw::server;

std::string document(std::string_view items = {}, std::string_view pack = "8",
                     std::string_view target = "windows-x64")
{
    return "{\"version\":1,\"name\":\"Example\",\"project\":[" + std::string{items} +
           "],\"configuration\":{\"abi\":{\"target\":\"" + std::string{target} +
           "\",\"pack\":" + std::string{pack} + "}}}";
}

bool load(std::string_view text, project_configuration& output, diagnostic_buffer& diagnostics)
{
    metrics_store metrics;
    return load_project_configuration(text, "C:/Root/project.json", operation_id{31},
                                      diagnostics, metrics, output).ok();
}

bool rejects(std::string_view text)
{
    project_configuration output;
    diagnostic_buffer diagnostics;
    return !load(text, output, diagnostics) && !diagnostics.empty();
}

bool test_valid_model_roles_paths_and_pack()
{
    project_configuration output;
    diagnostic_buffer diagnostics;
    if (!load(document(), output, diagnostics) || output.version != 1 ||
        output.name != "Example" || !output.project.empty() || output.abi.pack != 8)
        return false;

    const auto items =
        R"({"path":"types/../types/a.h","role":"type"},{"path":"src/main.cpp","role":"source"},{"path":"../Common/project.json","role":"project"})";
    diagnostics.clear();
    if (!load(document(items), output, diagnostics) || output.project.size() != 3)
        return false;
    if (output.project[0].role != project_item_role::type ||
        output.project[1].role != project_item_role::source ||
        output.project[2].role != project_item_role::project ||
        output.project[0].path != std::filesystem::path{"C:/Root/types/a.h"} ||
        output.project[2].path != std::filesystem::path{"C:/Common/project.json"})
        return false;

    constexpr std::string_view absolute_item =
        R"({"path":"D:/Sources/../Sources/main.cpp","role":"source"})";
    diagnostics.clear();
    if (!load(document(absolute_item), output, diagnostics) ||
        output.project[0].path != std::filesystem::path{"D:/Sources/main.cpp"}) return false;

    constexpr std::array packs{1, 2, 4, 8, 16};
    for (const auto pack : packs)
    {
        diagnostics.clear();
        if (!load(document({}, std::to_string(pack)), output, diagnostics) ||
            output.abi.pack != static_cast<std::uint32_t>(pack)) return false;
    }
    return true;
}

bool test_root_and_schema_failures()
{
    std::string excessive = R"({"version":1,"name":"x","project":)";
    excessive.append(20, '['); excessive += '0'; excessive.append(20, ']');
    excessive += R"(,"configuration":{"abi":{"target":"windows-x64","pack":8}}})";
    return rejects("{") && rejects("[]") && rejects("1") &&
        rejects(R"({"name":"x","project":[],"configuration":{"abi":{"target":"windows-x64","pack":8}}})") &&
        rejects(R"({"version":1,"project":[],"configuration":{"abi":{"target":"windows-x64","pack":8}}})") &&
        rejects(R"({"version":1,"name":"x","configuration":{"abi":{"target":"windows-x64","pack":8}}})") &&
        rejects(R"({"version":1,"name":"x","project":[]})") &&
        rejects(R"({"version":1,"version":1,"name":"x","project":[],"configuration":{"abi":{"target":"windows-x64","pack":8}}})") &&
        rejects(R"({"version":1,"name":"x","project":[],"extra":0,"configuration":{"abi":{"target":"windows-x64","pack":8}}})") &&
        rejects(R"({"version":"1","name":"x","project":[],"configuration":{"abi":{"target":"windows-x64","pack":8}}})") &&
        rejects(R"({"version":1,"name":[],"project":[],"configuration":{"abi":{"target":"windows-x64","pack":8}}})") &&
        rejects(R"({"version":1,"name":"x","project":{},"configuration":{"abi":{"target":"windows-x64","pack":8}}})") &&
        rejects(R"({"version":1,"name":"x","project":[],"configuration":[]})") &&
        rejects(excessive);
}

bool test_item_failures()
{
    const auto reject_items = [](std::string_view items) { return rejects(document(items)); };
    return reject_items("1") && reject_items(R"({"role":"source"})") &&
        reject_items(R"({"path":"","role":"source"})") &&
        reject_items(R"({"path":1,"role":"source"})") &&
        reject_items(R"({"path":"a"})") &&
        reject_items(R"({"path":"a","role":"other"})") &&
        reject_items(R"({"path":"a","role":1})") &&
        reject_items(R"({"path":"a","path":"b","role":"source"})") &&
        reject_items(R"({"path":"a","role":"source","extra":0})");
}

bool test_abi_version_and_transaction()
{
    if (!rejects(R"({"version":1,"name":"x","project":[],"configuration":{}})") ||
        !rejects(R"({"version":1,"name":"x","project":[],"configuration":{"abi":{"pack":8}}})") ||
        !rejects(document({}, "8", "linux-x64")) ||
        !rejects(R"({"version":1,"name":"x","project":[],"configuration":{"abi":{"target":1,"pack":8}}})") ||
        !rejects(R"({"version":1,"name":"x","project":[],"configuration":{"abi":{"target":"windows-x64"}}})") ||
        !rejects(document({}, "3")) || !rejects(document({}, "0")) ||
        !rejects(document({}, "-1")) || !rejects(document({}, "true")) ||
        !rejects(R"({"version":1,"name":"x","project":[],"configuration":{"abi":{"target":"windows-x64","pack":8,"pack":8}}})") ||
        !rejects(R"({"version":1,"name":"x","project":[],"configuration":{"abi":{"target":"windows-x64","pack":8,"extra":0}}})") ||
        !rejects(R"({"version":2,"name":"x","project":[],"configuration":{"abi":{"target":"windows-x64","pack":8}}})"))
        return false;

    project_configuration output;
    output.version = 77; output.name = "unchanged";
    diagnostic_buffer diagnostics;
    return !load("{}", output, diagnostics) && output.version == 77 && output.name == "unchanged";
}

bool test_ten_thousand_items()
{
    std::string items;
    for (int index = 0; index < 10000; ++index)
    {
        if (index != 0) items += ',';
        items += "{\"path\":\"src/" + std::to_string(index) + ".cpp\",\"role\":\"source\"}";
    }
    project_configuration output;
    diagnostic_buffer diagnostics;
    if (!load(document(items), output, diagnostics) || output.project.size() != 10000)
        return false;
    return output.project.front().path.filename() == "0.cpp" &&
           output.project[5000].path.filename() == "5000.cpp" &&
           output.project.back().path.filename() == "9999.cpp";
}

bool test_metrics()
{
    project_configuration output;
    diagnostic_buffer diagnostics;
    metrics_store metrics;
    const auto text = document(R"({"path":"a.cpp","role":"source"})");
    if (!load_project_configuration(text, "C:/Root/project.json", operation_id{32},
                                    diagnostics, metrics, output).ok()) return false;
    diagnostics.clear();
    static_cast<void>(load_project_configuration("{}", "C:/Root/project.json", operation_id{33},
                                                  diagnostics, metrics, output));
    diagnostics.clear();
    static_cast<void>(load_project_configuration_file(
        "C:/definitely-missing/project.json", operation_id{34}, diagnostics, metrics, output));
    const auto snapshot = metrics.snapshot();
    return snapshot.counter(metric_id::project_configuration_load_count).value == 3 &&
           snapshot.counter(metric_id::project_configuration_item_count).value == 1 &&
           snapshot.duration(metric_id::project_configuration_load_duration).count == 3 &&
           snapshot.duration(metric_id::project_configuration_parse_duration).count == 2 &&
           snapshot.duration(metric_id::project_configuration_path_resolution_duration).count == 1;
}
}

int main()
{
    return test_valid_model_roles_paths_and_pack() && test_root_and_schema_failures() &&
           test_item_failures() && test_abi_version_and_transaction() &&
           test_ten_thousand_items() && test_metrics() ? 0 : 1;
}
