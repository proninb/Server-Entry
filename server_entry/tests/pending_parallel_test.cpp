#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"

#include <cassert>
#include <iostream>

using namespace cw::server;

int main() {
    graph_manager manager;
    abi_configuration abi{abi_target::posix_x64, 8};
    assert(manager.initialize(abi).ok());

    auto transaction = manager.begin_build();
    assert(transaction.sources().add("a.cpp", project_item_role::source).ok());
    assert(transaction.sources().add("b.cpp", project_item_role::source).ok());
    const auto roots = transaction.sources().roots();

    string_id A, B, member_a, member_b;
    assert(transaction.strings().intern("A", A).ok());
    assert(transaction.strings().intern("B", B).ok());
    assert(transaction.strings().intern("ma", member_a).ok());
    assert(transaction.strings().intern("mb", member_b).ok());

    // Graph mutation is single-owner in v2. Pending canonical references are
    // still allowed: A may reference canonical B before B is materialized.
    graph_update::source_replacement replacement_a;
    assert(transaction.graph_state().replace_source(roots[0].source, replacement_a).ok());
    stable_id a_id;
    type_handle a_type;
    assert(replacement_a.add_named_type(
        A, aggregate_definition_state::defined, a_id, a_type).ok());
    member_build a_member{member_a, std::nullopt, B, 0, 0};
    assert(replacement_a.define_members(
        a_type, std::span<const member_build>{&a_member, 1}, {}).ok());

    graph_update::source_replacement replacement_b;
    assert(transaction.graph_state().replace_source(roots[1].source, replacement_b).ok());
    stable_id b_id;
    type_handle b_type;
    assert(replacement_b.add_named_type(
        B, aggregate_definition_state::defined, b_id, b_type).ok());
    member_build b_member{member_b, std::nullopt, A, 0, 0};
    assert(replacement_b.define_members(
        b_type, std::span<const member_build>{&b_member, 1}, {}).ok());

    assert(transaction.commit().ok());

    assert(manager.compiled_graph().find_id(A).value() == 1);
    assert(manager.compiled_graph().find_id(B).value() == 2);

    const auto* a = manager.compiled_graph().find(A);
    const auto* b = manager.compiled_graph().find(B);
    assert(a && b);

    const auto a_members = manager.compiled_graph().members(a->type);
    const auto b_members = manager.compiled_graph().members(b->type);
    assert(a_members.size() == 1 && b_members.size() == 1);

    type_handle resolved;
    assert(manager.compiled_graph().named(a_members[0].type, resolved));
    assert(resolved == b->type);
    assert(manager.compiled_graph().named(b_members[0].type, resolved));
    assert(resolved == a->type);

    // An unresolved canonical name must still fail closed at publication.
    graph_manager bad;
    assert(bad.initialize(abi).ok());
    auto bad_transaction = bad.begin_build();
    assert(bad_transaction.sources().add("x.cpp", project_item_role::source).ok());

    string_id X, Y, member;
    assert(bad_transaction.strings().intern("X", X).ok());
    assert(bad_transaction.strings().intern("Y", Y).ok());
    assert(bad_transaction.strings().intern("m", member).ok());

    graph_update::source_replacement replacement;
    assert(bad_transaction.graph_state().replace_source(
        bad_transaction.sources().roots()[0].source, replacement).ok());

    stable_id entity;
    type_handle type;
    assert(replacement.add_named_type(
        X, aggregate_definition_state::defined, entity, type).ok());
    member_build unresolved{member, std::nullopt, Y, 0, 0};
    assert(replacement.define_members(
        type, std::span<const member_build>{&unresolved, 1}, {}).ok());

    assert(!bad_transaction.commit().ok());
    assert(bad.state() == project_state::error);

    std::cout << "PASS\n";
}
