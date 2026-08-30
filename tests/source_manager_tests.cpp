#include "../server_entry/project/source/source_manager.hpp"

#include <filesystem>
#include <utility>

namespace
{
using namespace cw::server;

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
}

int main()
{
    return test_dedup_order_and_roles() && test_discard_and_replace_commit() &&
           test_stable_ids_across_updates() && test_rejects_non_source_declarations() &&
           test_rejects_stale_update() && test_move_invalidates_source_handle() ? 0 : 1;
}
