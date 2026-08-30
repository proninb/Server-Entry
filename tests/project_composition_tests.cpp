#include "../server_entry/project/project_composition_resolver.hpp"
#include "../server_entry/project/project_configuration_loader.hpp"
#include "../server_entry/diagnostics/diagnostic_descriptor.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>

namespace
{
using namespace cw::server;

std::string config(std::string_view name, std::string_view items = {}, int pack = 8)
{
    return "{\"version\":1,\"name\":\"" + std::string{name} +
        "\",\"project\":[" + std::string{items} +
        "],\"configuration\":{\"abi\":{\"target\":\"windows-x64\",\"pack\":" +
        std::to_string(pack) + "}}}";
}

bool write(const std::filesystem::path& path, std::string_view text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream stream{path, std::ios::binary};
    stream << text;
    return static_cast<bool>(stream);
}

struct temporary_tree
{
    std::filesystem::path path;
    temporary_tree()
    {
        path = std::filesystem::temp_directory_path() /
            ("server-entry-composition-" + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(++sequence));
        std::filesystem::create_directories(path);
    }
    ~temporary_tree() { std::error_code error; std::filesystem::remove_all(path, error); }
    inline static int sequence = 0;
};

bool resolve(const std::filesystem::path& root, project_configuration& configuration,
             resolved_project_composition& composition, diagnostic_buffer& diagnostics,
             project_composition_statistics* statistics = nullptr,
             metrics_store* supplied_metrics = nullptr)
{
    metrics_store local_metrics;
    auto& metrics = supplied_metrics ? *supplied_metrics : local_metrics;
    if (!load_project_configuration_file(root, operation_id{40}, diagnostics,
                                         metrics, configuration).ok())
        return false;
    diagnostics.clear();
    return resolve_project_composition(root, configuration, operation_id{41}, diagnostics, metrics,
                                       composition, statistics).ok();
}

bool test_root_nested_deterministic_and_abi()
{
    temporary_tree tree;
    if (!write(tree.path / "project.json", config("Root",
        R"({"path":"root.cpp","role":"source"},{"path":"A/project.json","role":"project"},{"path":"B/project.json","role":"project"})", 16)) ||
        !write(tree.path / "A/project.json", config("A",
        R"({"path":"a.h","role":"type"},{"path":"Nested/project.json","role":"project"})", 4)) ||
        !write(tree.path / "A/Nested/project.json", config("Nested", R"({"path":"n.cpp","role":"source"})")) ||
        !write(tree.path / "B/project.json", config("B", R"({"path":"b.cpp","role":"source"})"))) return false;

    project_configuration root;
    resolved_project_composition composition;
    diagnostic_buffer diagnostics;
    if (!resolve(tree.path / "project.json", root, composition, diagnostics) ||
        root.abi.pack != 16 || composition.items.size() != 4) return false;
    return composition.items[0].path.filename() == "root.cpp" &&
           composition.items[1].path.filename() == "a.h" &&
           composition.items[2].path.filename() == "n.cpp" &&
           composition.items[3].path.filename() == "b.cpp";
}

bool test_root_only()
{
    temporary_tree tree;
    if (!write(tree.path / "project.json", config("Root",
        R"({"path":"only.cpp","role":"source"})"))) return false;
    project_configuration root;
    resolved_project_composition composition;
    diagnostic_buffer diagnostics;
    project_composition_statistics statistics;
    metrics_store metrics;
    const auto root_path = tree.path / "project.json";
    if (!load_project_configuration_file(root_path, operation_id{40}, diagnostics,
                                         metrics, root).ok())
        return false;
    std::error_code error;
    std::filesystem::remove(root_path, error);
    diagnostics.clear();
    return !error && resolve_project_composition(root_path, root, operation_id{41}, diagnostics, metrics,
                                                  composition, &statistics).ok() &&
           root.name == "Root" && composition.items.size() == 1 &&
           statistics.configuration_files_loaded == 0;
}

bool test_shared_cache_and_bound()
{
    temporary_tree tree;
    write(tree.path / "project.json", config("Root",
        R"({"path":"A/project.json","role":"project"},{"path":"B/project.json","role":"project"})"));
    write(tree.path / "A/project.json", config("A", R"({"path":"../Common/project.json","role":"project"})"));
    write(tree.path / "B/project.json", config("B", R"({"path":"../Common/project.json","role":"project"})"));
    write(tree.path / "Common/project.json", config("Common", R"({"path":"common.cpp","role":"source"})"));
    project_configuration root;
    resolved_project_composition composition;
    diagnostic_buffer diagnostics;
    project_composition_statistics statistics;
    metrics_store metrics;
    if (!resolve(tree.path / "project.json", root, composition, diagnostics,
                 &statistics, &metrics)) return false;
    const auto snapshot = metrics.snapshot();
    return
           composition.items.size() == 1 && statistics.configuration_files_loaded == 3 &&
           statistics.max_active_workers <= statistics.worker_limit && statistics.worker_limit <= 8 &&
           snapshot.counter(metric_id::project_composition_resolve_count).value == 1 &&
           snapshot.counter(metric_id::project_composition_file_count).value == 3 &&
           snapshot.counter(metric_id::project_composition_cache_hit_count).value == 1 &&
           snapshot.duration(metric_id::project_composition_resolve_duration).count == 1 &&
           snapshot.gauge(metric_id::project_composition_max_parallel_workers).value <= 8;
}

bool cycle_case(std::string_view root_items, std::string_view a_items = {}, std::string_view b_items = {})
{
    temporary_tree tree;
    write(tree.path / "project.json", config("Root", root_items));
    if (!a_items.empty()) write(tree.path / "A/project.json", config("A", a_items));
    if (!b_items.empty()) write(tree.path / "B/project.json", config("B", b_items));
    project_configuration root;
    resolved_project_composition output; output.items.push_back({"unchanged", project_item_role::source});
    diagnostic_buffer diagnostics;
    return !resolve(tree.path / "project.json", root, output, diagnostics) &&
           root.name == "Root" && output.items.size() == 1 && !diagnostics.empty() &&
           diagnostics.records()[0].id == diagnostics::project_composition_cycle.id;
}

bool test_cycles()
{
    return cycle_case(R"({"path":"project.json","role":"project"})") &&
        cycle_case(R"({"path":"A/project.json","role":"project"})",
                   R"({"path":"../project.json","role":"project"})") &&
        cycle_case(R"({"path":"A/project.json","role":"project"})",
                   R"({"path":"../B/project.json","role":"project"})",
                   R"({"path":"../project.json","role":"project"})");
}

bool test_failures_and_many_siblings()
{
    temporary_tree missing;
    write(missing.path / "project.json", config("Root", R"({"path":"Missing/project.json","role":"project"})"));
    project_configuration root;
    resolved_project_composition composition;
    diagnostic_buffer diagnostics;
    if (resolve(missing.path / "project.json", root, composition, diagnostics) || diagnostics.empty()) return false;

    temporary_tree invalid;
    write(invalid.path / "project.json", config("Root", R"({"path":"Bad/project.json","role":"project"})"));
    write(invalid.path / "Bad/project.json", "{");
    diagnostics.clear();
    if (resolve(invalid.path / "project.json", root, composition, diagnostics)) return false;

    temporary_tree invalid_schema;
    write(invalid_schema.path / "project.json", config("Root", R"({"path":"Bad/project.json","role":"project"})"));
    write(invalid_schema.path / "Bad/project.json", "{}");
    diagnostics.clear();
    if (resolve(invalid_schema.path / "project.json", root, composition, diagnostics)) return false;

    temporary_tree many;
    std::string items;
    constexpr int count = 128;
    for (int index = 0; index < count; ++index)
    {
        if (index) items += ',';
        items += "{\"path\":\"P" + std::to_string(index) + "/project.json\",\"role\":\"project\"}";
        write(many.path / ("P" + std::to_string(index)) / "project.json",
              config("P", "{\"path\":\"item.cpp\",\"role\":\"source\"}"));
    }
    write(many.path / "project.json", config("Root", items));
    diagnostics.clear();
    project_composition_statistics statistics;
    return resolve(many.path / "project.json", root, composition, diagnostics, &statistics) &&
           composition.items.size() == count && statistics.configuration_files_loaded == count &&
           statistics.max_active_workers <= statistics.worker_limit;
}

bool test_deep_composition_uses_explicit_stack()
{
    temporary_tree tree;
    constexpr int depth = 2048;
    write(tree.path / "project.json", config("Root",
        R"({"path":"P0/project.json","role":"project"})"));
    for (int index = 0; index < depth; ++index)
    {
        const auto directory = tree.path / ("P" + std::to_string(index));
        if (index + 1 == depth)
        {
            if (!write(directory / "project.json",
                       config("Leaf", R"({"path":"leaf.cpp","role":"source"})")))
                return false;
        }
        else
        {
            const auto next = "../P" + std::to_string(index + 1) + "/project.json";
            const auto item = "{\"path\":\"" + next + "\",\"role\":\"project\"}";
            if (!write(directory / "project.json", config("Node", item))) return false;
        }
    }

    project_configuration root;
    resolved_project_composition composition;
    diagnostic_buffer diagnostics;
    project_composition_statistics statistics;
    return resolve(tree.path / "project.json", root, composition, diagnostics, &statistics) &&
           composition.items.size() == 1 && composition.items[0].path.filename() == "leaf.cpp" &&
           statistics.configuration_files_loaded == depth;
}
}

int main()
{
    return test_root_only() && test_root_nested_deterministic_and_abi() && test_shared_cache_and_bound() &&
           test_cycles() && test_failures_and_many_siblings() &&
           test_deep_composition_uses_explicit_stack() ? 0 : 1;
}
