#include "project/graph/compiled_state.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include "project/graph/graph_manager.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace cw::server;

static source_id ensure_source(graph_build_transaction& tx) {
    const auto roots = tx.sources().roots();
    if (!roots.empty()) {
        return roots[0].source;
    }

    assert(tx.sources().add("arena.cpp", project_item_role::source).ok());
    return tx.sources().roots()[0].source;
}

static void publish_definition(
    graph_build_transaction& tx,
    source_id source,
    std::size_t member_count,
    std::size_t enumerator_count) {

    string_id type_name;
    assert(tx.strings().intern("A", type_name).ok());

    std::vector<string_id> member_names(member_count);
    std::vector<member_build> members(member_count);

    for (std::size_t index = 0; index < member_count; ++index) {
        const auto spelling = "m" + std::to_string(index);
        assert(tx.strings().intern(spelling, member_names[index]).ok());
        members[index].name = member_names[index];
        members[index].builtin = builtin_type::integer;
    }

    string_id enum_name;
    assert(tx.strings().intern("E", enum_name).ok());

    std::vector<string_id> enumerator_names(enumerator_count);
    std::vector<enum_value_build> enumerators(enumerator_count);
    for (std::size_t index = 0; index < enumerator_count; ++index) {
        const auto spelling = "e" + std::to_string(index);
        assert(tx.strings().intern(spelling, enumerator_names[index]).ok());
        enumerators[index].name = enumerator_names[index];
        enumerators[index].value = {builtin_type::integer, index};
    }

    graph_update::source_replacement replacement;
    assert(tx.graph_state().replace_source(source, replacement).ok());

    stable_id entity;
    type_handle type;
    assert(replacement.add_named_type(
        type_name,
        aggregate_definition_state::defined,
        entity,
        type).ok());

    assert(replacement.define_members(type, members, {}).ok());

    enum_build_data enum_data;
    enum_data.definition_state = enum_definition_state::defined;
    enum_data.enumerators = enumerators;

    stable_id enum_entity;
    type_handle enum_type;
    assert(replacement.add_named_enum(
        enum_name, enum_data, enum_entity, enum_type).ok());
}

static compiled_graph_state persisted_state(const graph_manager& manager) {
    compiled_graph_state state;
    assert(manager.compiled_graph().export_compiled(state).ok());
    return state;
}

int main() {
    graph_manager manager;
    abi_configuration abi{};
    abi.target = abi_target::posix_x64;
    abi.pack = 8;
    assert(manager.initialize(abi).ok());

    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        const auto source = ensure_source(tx);
        publish_definition(tx, source, 1, 1);
        assert(tx.commit().ok());
    }
    { const auto state = persisted_state(manager); assert(state.members.size() == 1); assert(state.enum_values.size() == 1); }

    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        const auto source = ensure_source(tx);
        publish_definition(tx, source, 2, 2);
        assert(tx.commit().ok());
    }
    { const auto state = persisted_state(manager); assert(state.members.size() == 3); assert(state.enum_values.size() == 3); }

    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        const auto source = ensure_source(tx);
        publish_definition(tx, source, 3, 3);
        assert(tx.commit().ok());
    }
    { const auto state = persisted_state(manager); assert(state.members.size() == 6); assert(state.enum_values.size() == 6); }

    // Explicit Rebuild creates G0 from fresh compact definition arenas. Only the
    // current definition survives; obsolete slices from G1/G2 are reclaimed.
    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        const auto source = ensure_source(tx);
        publish_definition(tx, source, 2, 2);
        assert(tx.commit().ok());
    }
    { const auto state = persisted_state(manager); assert(state.members.size() == 2); assert(state.enum_values.size() == 2); }

    const auto* entity = manager.compiled_graph().find(manager.strings().find("A"));
    assert(entity);
    const auto members = manager.compiled_graph().members(entity->type);
    assert(members.size() == 2);

    std::cout << "PASS\n";
}
