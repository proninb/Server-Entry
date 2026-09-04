#include "project/graph/graph_build_transaction.hpp"
#include "project/graph/graph_manager.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

using namespace cw::server;

static source_id ensure_source(graph_build_transaction& tx) {
    const auto roots = tx.sources().roots();
    if (!roots.empty()) {
        return roots[0].source;
    }

    assert(tx.sources().add("strings.cpp", project_item_role::source).ok());
    return tx.sources().roots()[0].source;
}

static string_id publish_member(
    graph_build_transaction& tx,
    source_id source,
    const std::string& member_spelling) {

    string_id type_name;
    string_id member_name;
    assert(tx.strings().intern("A", type_name).ok());
    assert(tx.strings().intern(member_spelling, member_name).ok());

    graph_update::source_replacement replacement;
    assert(tx.graph_state().replace_source(source, replacement).ok());

    stable_id entity;
    type_handle type;
    assert(replacement.add_named_type(
        type_name,
        aggregate_definition_state::defined,
        entity,
        type).ok());

    member_build member;
    member.name = member_name;
    member.builtin = builtin_type::integer;
    assert(replacement.define_members(
        type,
        std::span<const member_build>{&member, 1},
        {}).ok());

    return member_name;
}

int main() {
    graph_manager manager;
    abi_configuration abi{};
    abi.target = abi_target::posix_x64;
    abi.pack = 8;
    assert(manager.initialize(abi).ok());

    string_id type_name_id;
    string_id old0;
    string_id old1;

    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        const auto source = ensure_source(tx);
        old0 = publish_member(tx, source, "old0");
        assert(tx.commit().ok());
        type_name_id = manager.strings().find("A");
        assert(type_name_id);
    }

    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        const auto source = ensure_source(tx);
        old1 = publish_member(tx, source, "old1");
        assert(tx.commit().ok());
    }

    string_id current_before_rebuild;
    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        const auto source = ensure_source(tx);
        current_before_rebuild = publish_member(tx, source, "current");
        assert(tx.commit().ok());
    }

    assert(manager.strings().get(old0));
    assert(manager.strings().get(old1));
    assert(manager.strings().get(current_before_rebuild));
    const auto slots_before = manager.strings().size();
    const auto live_before = manager.strings().live_size();

    const auto stable_before =
        manager.compiled_graph().find_id(type_name_id);
    assert(stable_before);

    // G0 preserves numeric string_id slots but reclaims bytes/lookup entries for
    // strings no longer referenced by current G or historical Entity identity.
    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        const auto source = ensure_source(tx);
        const auto current = publish_member(tx, source, "current");
        assert(current == current_before_rebuild);
        assert(tx.commit().ok());
    }

    assert(manager.strings().size() == slots_before);
    assert(manager.strings().live_size() < live_before);
    assert(!manager.strings().get(old0));
    assert(!manager.strings().get(old1));
    assert(!manager.strings().find("old0"));
    assert(!manager.strings().find("old1"));

    // Canonical Entity spelling is historical identity and must not be reclaimed.
    assert(manager.strings().find("A") == type_name_id);
    assert(manager.compiled_graph().find_id(type_name_id) == stable_before);
    assert(manager.strings().find("current") == current_before_rebuild);

    const auto checkpoint =
        std::filesystem::temp_directory_path() /
        "cw_server_entry_v2_string_tombstones.bin";

    assert(manager.save_compiled_checkpoint(checkpoint).ok());

    graph_manager loaded;
    assert(loaded.initialize(abi).ok());
    assert(loaded.load_compiled_checkpoint(checkpoint).ok());

    assert(loaded.strings().size() == manager.strings().size());
    assert(!loaded.strings().get(old0));
    assert(!loaded.strings().get(old1));
    assert(loaded.strings().find("A") == type_name_id);
    assert(loaded.compiled_graph().find_id(type_name_id) == stable_before);
    assert(loaded.strings().find("current") == current_before_rebuild);

    std::error_code ignored;
    std::filesystem::remove(checkpoint, ignored);

    std::cout << "PASS\n";
}
