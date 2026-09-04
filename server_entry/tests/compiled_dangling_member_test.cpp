#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include "project/graph/compiled_state.hpp"

#include <cassert>
#include <iostream>

using namespace cw::server;

int main() {
    graph_manager manager;
    abi_configuration abi{abi_target::posix_x64, 8};
    assert(manager.initialize(abi).ok());

    auto tx = manager.begin_build();
    assert(tx.sources().add("b.cpp", project_item_role::source).ok());
    assert(tx.sources().add("a.cpp", project_item_role::source).ok());
    const auto roots = tx.sources().roots();

    string_id A, B, member_name;
    assert(tx.strings().intern("A", A).ok());
    assert(tx.strings().intern("B", B).ok());
    assert(tx.strings().intern("b", member_name).ok());

    graph_update::source_replacement rb;
    assert(tx.graph_state().replace_source(roots[0].source, rb).ok());
    stable_id be;
    type_handle bt;
    assert(rb.add_named_type(B, aggregate_definition_state::declared, be, bt).ok());

    graph_update::source_replacement ra;
    assert(tx.graph_state().replace_source(roots[1].source, ra).ok());
    stable_id ae;
    type_handle at;
    assert(ra.add_named_type(A, aggregate_definition_state::defined, ae, at).ok());
    member_build member{member_name, std::nullopt, B, 0, 0};
    assert(ra.define_members(at, std::span<const member_build>{&member, 1}, {}).ok());
    assert(tx.commit().ok());

    compiled_graph_state state;
    assert(manager.compiled_graph().export_compiled(state).ok());

    const auto* b_entity = manager.compiled_graph().find(B);
    assert(b_entity);
    const auto b_id = manager.compiled_graph().find_id(B).value();
    const auto b_type = b_entity->type.value();

    state.entities[b_id] = {};
    state.types[b_type - 1].live = false;
    state.identities[B.value()] = 0;

    graph imported;
    assert(imported.initialize(abi).ok());
    const auto result = imported.import_compiled(state);
    assert(!result.ok());
    assert(result.code == status_code::artifact_corrupt);

    std::cout << "PASS\n";
}
