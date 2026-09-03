#include "../server_entry/project/source/source_manager.hpp"
#include "../server_entry/diagnostics/diagnostic_buffer.hpp"
#include "../server_entry/diagnostics/diagnostic_descriptor.hpp"
#include "../server_entry/operation.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace cw::server;

class temporary_source_file
{
public:
    explicit temporary_source_file(std::string_view name)
        : path(std::filesystem::absolute(std::filesystem::path{"out"} / name)
                   .lexically_normal())
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        valid = static_cast<bool>(output);
    }
    ~temporary_source_file()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    std::filesystem::path path;
    bool valid = false;
};

bool test_dedup_order_and_roles()
{
    source_manager manager;
    auto update = manager.begin_update();
    if (!update.add("C:/Root/src/a.cpp", project_item_role::source).ok() ||
        !update.add("C:/Root/src/a.cpp", project_item_role::type).ok() ||
        !update.add("C:/Root/src/b.cpp", project_item_role::source).ok() ||
        !update.commit().ok()) return false;
    if (manager.sources().size() != 2 || manager.roots().size() != 3 ||
        manager.roots()[0].source != manager.roots()[1].source ||
        manager.roots()[0].role != project_item_role::source ||
        manager.roots()[1].role != project_item_role::type) return false;
    const auto* first = manager.find(manager.roots()[0].source);
    return first && first->path == std::filesystem::path{"C:/Root/src/a.cpp"};
}

bool test_discard_and_replace_commit()
{
    source_manager manager;
    { auto discarded = manager.begin_update(); if (!discarded.add("C:/Root/discarded.cpp", project_item_role::source).ok()) return false; }
    if (!manager.roots().empty()) return false;
    { auto first = manager.begin_update(); if (!first.add("C:/Root/first.cpp", project_item_role::source).ok() || !first.commit().ok()) return false; }
    { auto second = manager.begin_update(); if (!second.add("C:/Root/second.h", project_item_role::type).ok() || !second.commit().ok()) return false; }
    return manager.sources().size() == 2 && manager.roots().size() == 1 &&
           manager.find(manager.roots()[0].source)->path.filename() == "second.h";
}

bool test_stable_ids_across_updates()
{
    source_manager manager;
    source_id a, b, c;
    {
        auto update = manager.begin_update();
        if (!update.add("C:/Root/A.cpp", project_item_role::source).ok() ||
            !update.add("C:/Root/B.cpp", project_item_role::source).ok() ||
            !update.add("C:/Root/C.cpp", project_item_role::source).ok() || !update.commit().ok()) return false;
        a = manager.roots()[0].source; b = manager.roots()[1].source; c = manager.roots()[2].source;
    }
    {
        auto update = manager.begin_update();
        if (!update.add("C:/Root/C.cpp", project_item_role::source).ok() ||
            !update.add("C:/Root/A.cpp", project_item_role::source).ok() ||
            !update.add("C:/Root/B.cpp", project_item_role::source).ok() || !update.commit().ok()) return false;
        if (manager.roots()[0].source != c || manager.roots()[1].source != a ||
            manager.roots()[2].source != b) return false;
    }
    {
        auto update = manager.begin_update();
        if (!update.add("C:/Root/B.cpp", project_item_role::source).ok() ||
            !update.add("C:/Root/C.cpp", project_item_role::source).ok() || !update.commit().ok()) return false;
    }
    if (manager.sources().size() != 3 || manager.find(a)->path.filename() != "A.cpp") return false;
    auto update = manager.begin_update();
    return update.add("C:/Root/A.cpp", project_item_role::type).ok() && update.commit().ok() &&
           manager.roots()[0].source == a && manager.sources().size() == 3;
}

bool test_rejects_non_source_declarations()
{
    source_manager manager;
    auto empty = manager.begin_update();
    if (empty.add({}, project_item_role::source).code != status_code::configuration_failed ||
        empty.commit().ok() || !manager.roots().empty()) return false;
    auto project = manager.begin_update();
    return project.add("C:/Root/project.json", project_item_role::project).code ==
               status_code::configuration_failed &&
           !project.commit().ok() && manager.roots().empty();
}

bool test_rejects_stale_update()
{
    source_manager manager;
    auto first = manager.begin_update();
    auto stale = manager.begin_update();
    if (!first.add("C:/Root/first.cpp", project_item_role::source).ok() || !first.commit().ok() ||
        !stale.add("C:/Root/stale.cpp", project_item_role::source).ok()) return false;
    return stale.commit().code == status_code::invalid_state &&
           manager.sources().size() == 1 && manager.roots().size() == 1 &&
           manager.find(manager.roots()[0].source)->path.filename() == "first.cpp";
}

bool test_move_invalidates_source_handle()
{
    source_manager manager;
    auto source = manager.begin_update();
    if (!source.add("C:/Root/moved.cpp", project_item_role::source).ok()) return false;
    auto destination = std::move(source);
    if (source.add("C:/Root/invalid.cpp", project_item_role::source).code != status_code::invalid_state ||
        source.commit().code != status_code::invalid_state) return false;
    return destination.commit().ok() && manager.sources().size() == 1 &&
           manager.roots().size() == 1 &&
           manager.find(manager.roots()[0].source)->path.filename() == "moved.cpp";
}

bool test_include_identity_and_reverse_edges()
{
    temporary_source_file b_file{"source_manager_B.noc"};
    if (!b_file.valid) return false;
    source_manager manager;
    auto update = manager.begin_update();
    source_id a, b, c;
    if (!update.resolve("C:/Root/A.noc", project_item_role::source, a).ok() ||
        !update.resolve_include(b_file.path, b).ok() ||
        !update.resolve("C:/Root/C.noc", project_item_role::source, c).ok() ||
        !update.set_includes(a, std::span<const source_id>{&b, 1}).ok() ||
        !update.set_includes(c, std::span<const source_id>{&b, 1}).ok()) return false;
    diagnostic_buffer diagnostics;
    if (!update.validate_source_graph(operation_id{1}, diagnostics).ok() ||
        !update.commit().ok()) return false;
    if (manager.sources().size() != 3 || manager.roots().size() != 2 ||
        manager.includes(a).size() != 1 || manager.includes(a)[0] != b ||
        manager.dependents(b).size() != 2) return false;

    auto next = manager.begin_update();
    source_id root_b;
    if (!next.resolve(b_file.path, project_item_role::source, root_b).ok() ||
        root_b != b || !next.commit().ok()) return false;
    return manager.sources().size() == 3 && manager.roots().size() == 1 &&
           manager.roots()[0].source == b;
}

bool test_transitive_reverse_dependencies()
{
    temporary_source_file b_file{"source_manager_transitive_B.noc"};
    temporary_source_file c_file{"source_manager_transitive_C.noc"};
    temporary_source_file d_file{"source_manager_transitive_D.noc"};
    if (!b_file.valid || !c_file.valid || !d_file.valid) return false;
    source_manager manager;
    auto update = manager.begin_update();
    source_id a, b, c, d;
    if (!update.resolve("C:/Root/A.noc", project_item_role::source, a).ok() ||
        !update.resolve_include(b_file.path, b).ok() ||
        !update.resolve_include(c_file.path, c).ok() ||
        !update.resolve_include(d_file.path, d).ok()) return false;
    const source_id ab[]{b};
    const source_id bc[]{c};
    const source_id cd[]{d};
    if (!update.set_includes(a, ab).ok() || !update.set_includes(b, bc).ok() ||
        !update.set_includes(c, cd).ok()) return false;
    diagnostic_buffer diagnostics;
    if (!update.validate_source_graph(operation_id{2}, diagnostics).ok() ||
        !update.commit().ok()) return false;
    std::vector<source_id> affected;
    if (!manager.collect_dependents(d, affected).ok() || affected.size() != 3)
        return false;
    return std::find(affected.begin(), affected.end(), a) != affected.end() &&
           std::find(affected.begin(), affected.end(), b) != affected.end() &&
           std::find(affected.begin(), affected.end(), c) != affected.end();
}

bool test_include_cycle_rejected()
{
    temporary_source_file b_file{"source_manager_cycle_B.noc"};
    if (!b_file.valid) return false;
    source_manager manager;
    auto update = manager.begin_update();
    source_id a, b;
    if (!update.resolve("C:/Root/A.noc", project_item_role::source, a).ok() ||
        !update.resolve_include(b_file.path, b).ok()) return false;
    const source_id ab[]{b};
    const source_id ba[]{a};
    if (!update.set_includes(a, ab).ok() || !update.set_includes(b, ba).ok())
        return false;
    diagnostic_buffer diagnostics;
    const auto result = update.validate_source_graph(operation_id{3}, diagnostics);
    return result.code == status_code::configuration_failed &&
           update.commit().code == status_code::configuration_failed &&
           manager.sources().empty() && diagnostics.records().size() == 1 &&
           diagnostics.records()[0].id == diagnostics::source_include_cycle.id;
}
}

int main()
{
    const std::pair<const char*, bool(*)()> tests[] = {
        {"dedup", test_dedup_order_and_roles},
        {"discard", test_discard_and_replace_commit},
        {"stable_ids", test_stable_ids_across_updates},
        {"roles", test_rejects_non_source_declarations},
        {"stale", test_rejects_stale_update},
        {"move", test_move_invalidates_source_handle},
        {"include_identity", test_include_identity_and_reverse_edges},
        {"transitive", test_transitive_reverse_dependencies},
        {"cycle", test_include_cycle_rejected}};
    for (const auto& [name, test] : tests)
        if (!test()) { std::cerr << name << " failed\n"; return 1; }
    return 0;
}
