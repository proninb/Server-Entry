#include "../server_entry/project/project_composition_resolver.hpp"
#include "../server_entry/project/project_configuration_loader.hpp"
#include "../server_entry/project/source/source_manager.hpp"
#include "../server_entry/diagnostics/diagnostic_descriptor.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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
    std::ofstream stream{path, std::ios::binary}; stream << text;
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

const std::filesystem::path& root_path(const source_manager& sources, std::size_t index)
{
    return sources.find(sources.roots()[index].source)->path;
}

bool resolve(const std::filesystem::path& path, project_configuration& configuration,
             source_manager& sources, diagnostic_buffer& diagnostics,
             project_composition_statistics* statistics = nullptr,
             metrics_store* supplied_metrics = nullptr)
{
    metrics_store local_metrics;
    auto& metrics = supplied_metrics ? *supplied_metrics : local_metrics;
    if (!load_project_configuration_file(path, operation_id{40}, diagnostics,
                                         metrics, configuration).ok()) return false;
    diagnostics.clear();
    auto update = sources.begin_update();
    const auto result = resolve_project_composition(path, configuration, operation_id{41},
                                                     diagnostics, metrics, update, statistics);
    return result.ok() && update.commit().ok();
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
    project_configuration root; source_manager sources; diagnostic_buffer diagnostics;
    if (!resolve(tree.path / "project.json", root, sources, diagnostics) ||
        root.abi.pack != 16 || sources.roots().size() != 4) return false;
    return root_path(sources, 0).filename() == "root.cpp" &&
           root_path(sources, 1).filename() == "a.h" &&
           root_path(sources, 2).filename() == "n.cpp" &&
           root_path(sources, 3).filename() == "b.cpp" &&
           sources.roots()[1].role == project_item_role::type;
}

bool test_root_only_and_no_reload()
{
    temporary_tree tree;
    const auto path = tree.path / "project.json";
    write(path, config("Root", R"({"path":"only.cpp","role":"source"})"));
    project_configuration root; source_manager sources; diagnostic_buffer diagnostics;
    metrics_store metrics; project_composition_statistics statistics;
    if (!load_project_configuration_file(path, operation_id{40}, diagnostics, metrics, root).ok()) return false;
    std::error_code error; std::filesystem::remove(path, error);
    auto update = sources.begin_update();
    const auto result = resolve_project_composition(path, root, operation_id{41}, diagnostics,
                                                     metrics, update, &statistics);
    return !error && result.ok() && update.commit().ok() && sources.roots().size() == 1 &&
           root_path(sources, 0).filename() == "only.cpp" &&
           statistics.configuration_files_loaded == 0;
}

bool test_shared_cache_metrics_and_path_dedup()
{
    temporary_tree tree;
    write(tree.path / "project.json", config("Root",
        R"({"path":"A/project.json","role":"project"},{"path":"B/project.json","role":"project"})"));
    write(tree.path / "A/project.json", config("A", R"({"path":"../Common/project.json","role":"project"})"));
    write(tree.path / "B/project.json", config("B", R"({"path":"../Common/project.json","role":"project"})"));
    write(tree.path / "Common/project.json", config("Common",
        R"({"path":"common.cpp","role":"source"},{"path":"./common.cpp","role":"type"})"));
    project_configuration root; source_manager sources; diagnostic_buffer diagnostics;
    project_composition_statistics statistics; metrics_store metrics;
    if (!resolve(tree.path / "project.json", root, sources, diagnostics, &statistics, &metrics)) return false;
    const auto snapshot = metrics.snapshot();
    return sources.sources().size() == 1 && sources.roots().size() == 2 &&
           sources.roots()[0].source == sources.roots()[1].source &&
           statistics.configuration_files_loaded == 3 &&
           snapshot.counter(metric_id::project_composition_cache_hit_count).value == 1;
}

bool cycle_case(std::string_view root_items, std::string_view a_items = {},
                std::string_view b_items = {})
{
    temporary_tree tree;
    write(tree.path / "project.json", config("Root", root_items));
    if (!a_items.empty()) write(tree.path / "A/project.json", config("A", a_items));
    if (!b_items.empty()) write(tree.path / "B/project.json", config("B", b_items));
    source_manager sources;
    { auto initial = sources.begin_update(); if (!initial.add("unchanged.cpp", project_item_role::source).ok() || !initial.commit().ok()) return false; }
    project_configuration root; diagnostic_buffer diagnostics; metrics_store metrics;
    if (!load_project_configuration_file(tree.path / "project.json", operation_id{40}, diagnostics,
                                         metrics, root).ok()) return false;
    auto update = sources.begin_update();
    const auto result = resolve_project_composition(tree.path / "project.json", root,
        operation_id{41}, diagnostics, metrics, update);
    return !result.ok() && sources.roots().size() == 1 &&
           root_path(sources, 0).filename() == "unchanged.cpp" && !diagnostics.empty() &&
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

bool test_subproject_failures_do_not_commit()
{
    const auto run = [](std::string_view child_text)
    {
        temporary_tree tree;
        write(tree.path / "project.json", config("Root",
            R"({"path":"Bad/project.json","role":"project"})"));
        if (!child_text.empty()) write(tree.path / "Bad/project.json", child_text);
        source_manager sources;
        { auto initial = sources.begin_update(); if (!initial.add("unchanged.cpp", project_item_role::source).ok() || !initial.commit().ok()) return false; }
        project_configuration root; diagnostic_buffer diagnostics; metrics_store metrics;
        if (!load_project_configuration_file(tree.path / "project.json", operation_id{40},
                                             diagnostics, metrics, root).ok()) return false;
        auto update = sources.begin_update();
        const auto result = resolve_project_composition(tree.path / "project.json", root,
            operation_id{41}, diagnostics, metrics, update);
        return !result.ok() && sources.roots().size() == 1 &&
               root_path(sources, 0).filename() == "unchanged.cpp" && !diagnostics.empty();
    };
    return run({}) && run("{") && run("{}");
}

bool test_many_and_deep()
{
    temporary_tree many; std::string items; constexpr int count = 128;
    for (int index = 0; index < count; ++index)
    {
        if (index) items += ',';
        items += "{\"path\":\"P" + std::to_string(index) + "/project.json\",\"role\":\"project\"}";
        write(many.path / ("P" + std::to_string(index)) / "project.json",
              config("P", R"({"path":"item.cpp","role":"source"})"));
    }
    write(many.path / "project.json", config("Root", items));
    project_configuration root; source_manager sources; diagnostic_buffer diagnostics;
    project_composition_statistics statistics;
    if (!resolve(many.path / "project.json", root, sources, diagnostics, &statistics) ||
        sources.roots().size() != count || statistics.configuration_files_loaded != count) return false;

    temporary_tree deep; constexpr int depth = 2048;
    write(deep.path / "project.json", config("Root", R"({"path":"P0/project.json","role":"project"})"));
    for (int index = 0; index < depth; ++index)
    {
        const auto directory = deep.path / ("P" + std::to_string(index));
        if (index + 1 == depth)
            write(directory / "project.json", config("Leaf", R"({"path":"leaf.cpp","role":"source"})"));
        else
        {
            const auto item = "{\"path\":\"../P" + std::to_string(index + 1) +
                              "/project.json\",\"role\":\"project\"}";
            write(directory / "project.json", config("Node", item));
        }
    }
    source_manager deep_sources; diagnostics.clear();
    return resolve(deep.path / "project.json", root, deep_sources, diagnostics, &statistics) &&
           deep_sources.roots().size() == 1 && root_path(deep_sources, 0).filename() == "leaf.cpp";
}
} // namespace

int main()
{
    return test_root_only_and_no_reload() && test_root_nested_deterministic_and_abi() &&
           test_shared_cache_metrics_and_path_dedup() && test_cycles() &&
           test_subproject_failures_do_not_commit() && test_many_and_deep() ? 0 : 1;
}
