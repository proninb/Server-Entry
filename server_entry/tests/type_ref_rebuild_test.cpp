#include "project/graph/compiled_state.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include "project/graph/graph_manager.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace cw::server;

static source_id ensure_source(graph_build_transaction& tx) {
    const auto roots = tx.sources().roots();
    if (!roots.empty()) {
        return roots[0].source;
    }

    assert(tx.sources().add("types.cpp", project_item_role::source).ok());
    return tx.sources().roots()[0].source;
}

static void publish_types(
    graph_build_transaction& tx,
    source_id source,
    std::span<const type_modifier_build> modifiers) {

    string_id a;
    string_id b;
    string_id member_name;
    assert(tx.strings().intern("A", a).ok());
    assert(tx.strings().intern("B", b).ok());
    assert(tx.strings().intern("value", member_name).ok());

    graph_update::source_replacement replacement;
    assert(tx.graph_state().replace_source(source, replacement).ok());

    stable_id a_id;
    stable_id b_id;
    type_handle a_type;
    type_handle b_type;

    assert(replacement.add_named_type(
        a,
        aggregate_definition_state::defined,
        a_id,
        a_type).ok());

    member_build member;
    member.name = member_name;
    member.user_type_name = b;
    member.modifier_offset = 0;
    member.modifier_count = static_cast<std::uint32_t>(modifiers.size());

    assert(replacement.define_members(
        a_type,
        std::span<const member_build>{&member, 1},
        modifiers).ok());

    assert(replacement.add_named_type(
        b,
        aggregate_definition_state::declared,
        b_id,
        b_type).ok());
}

static compiled_graph_state exported(const graph_manager& manager) {
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

    const type_modifier_build pointer[] {
        {derived_type_kind::pointer, 0}
    };

    const type_modifier_build pointer_array[] {
        {derived_type_kind::pointer, 0},
        {derived_type_kind::array, 4}
    };

    const type_modifier_build pointer_array_ref[] {
        {derived_type_kind::pointer, 0},
        {derived_type_kind::array, 4},
        {derived_type_kind::lvalue_reference, 0}
    };

    stable_id original_a;
    stable_id original_b;

    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        const auto source = ensure_source(tx);
        publish_types(tx, source, pointer);
        assert(tx.commit().ok());

        original_a = manager.compiled_graph().find_id(manager.strings().find("A"));
        original_b = manager.compiled_graph().find_id(manager.strings().find("B"));
        assert(original_a && original_b);
        assert(manager.compiled_graph().derived_type_count() == 1);
    }

    const auto initial_size = exported(manager).canonical_types.size();

    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        const auto source = ensure_source(tx);
        publish_types(tx, source, pointer_array);
        assert(tx.commit().ok());
        assert(manager.compiled_graph().derived_type_count() >= 2);
    }

    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        const auto source = ensure_source(tx);
        publish_types(tx, source, pointer_array_ref);
        assert(tx.commit().ok());
        assert(manager.compiled_graph().derived_type_count() >= 3);
    }

    const auto grown_size = exported(manager).canonical_types.size();
    assert(grown_size > initial_size);

    // Explicit Rebuild creates a new G0 TypeRef namespace. Only live named types
    // and the currently reachable pointer(B) derived chain survive compaction.
    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        const auto source = ensure_source(tx);
        publish_types(tx, source, pointer);
        assert(tx.commit().ok());
    }

    const auto compact = exported(manager);
    assert(manager.compiled_graph().derived_type_count() == 1);
    assert(compact.canonical_types.size() == initial_size);
    assert(compact.canonical_types.size() < grown_size);

    assert(manager.compiled_graph().find_id(manager.strings().find("A")) == original_a);
    assert(manager.compiled_graph().find_id(manager.strings().find("B")) == original_b);

    const auto* a = manager.compiled_graph().find(original_a);
    assert(a);
    const auto members = manager.compiled_graph().members(a->type);
    assert(members.size() == 1);

    const auto* derived = manager.compiled_graph().derived(members[0].type);
    assert(derived && derived->kind == derived_type_kind::pointer);

    type_handle named;
    assert(manager.compiled_graph().named(derived->child, named));
    const auto* b = manager.compiled_graph().find(original_b);
    assert(b && named == b->type);

    std::cout << "PASS\n";
}
