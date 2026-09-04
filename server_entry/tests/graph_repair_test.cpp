#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include <cassert>
#include <iostream>
#include <thread>

using namespace cw::server;

static void seed_sources(graph_build_transaction& tx, int count) {
    for (int i = 0; i < count; ++i) {
        auto path = std::filesystem::path{"source" + std::to_string(i) + ".cpp"};
        assert(tx.sources().add(path, project_item_role::source).ok());
    }
}

static std::pair<std::uint32_t,std::uint32_t> build_order(bool reverse) {
    graph_manager gm;
    abi_configuration abi{}; abi.target = abi_target::posix_x64; abi.pack = 8;
    assert(gm.initialize(abi).ok());
    auto tx = gm.begin_build();
    seed_sources(tx, 1);
    auto source = tx.sources().roots()[0].source;

    string_id a,z;
    assert(tx.strings().intern("A", a).ok());
    assert(tx.strings().intern("Z", z).ok());

    graph_update::source_replacement replacement;
    assert(tx.graph_state().replace_source(source, replacement).ok());
    stable_id e1,e2; type_handle t1,t2;
    if (reverse) {
        assert(replacement.add_named_type(z, aggregate_definition_state::declared, e1, t1).ok());
        assert(replacement.add_named_type(a, aggregate_definition_state::declared, e2, t2).ok());
    } else {
        assert(replacement.add_named_type(a, aggregate_definition_state::declared, e1, t1).ok());
        assert(replacement.add_named_type(z, aggregate_definition_state::declared, e2, t2).ok());
    }
    assert(tx.commit().ok());
    const auto* ae = gm.compiled_graph().find(gm.strings().find("A"));
    const auto* ze = gm.compiled_graph().find(gm.strings().find("Z"));
    assert(ae && ze);
    return {gm.compiled_graph().find_id(a).value(), gm.compiled_graph().find_id(z).value()};
}

int main() {
    auto left = build_order(false);
    auto right = build_order(true);
    assert(left == right);
    assert(left.first < left.second); // lexical A before Z

    graph_manager gm;
    abi_configuration abi{}; abi.target = abi_target::posix_x64; abi.pack = 8;
    assert(gm.initialize(abi).ok());
    auto tx = gm.begin_build();
    seed_sources(tx, 1);
    const auto source = tx.sources().roots()[0].source;

    string_id a,b,m;
    assert(tx.strings().intern("A", a).ok());
    assert(tx.strings().intern("B", b).ok());
    assert(tx.strings().intern("m", m).ok());

    graph_update::source_replacement replacement;
    assert(tx.graph_state().replace_source(source, replacement).ok());
    stable_id ae,be; type_handle at,bt;
    assert(replacement.add_named_type(a, aggregate_definition_state::defined, ae, at).ok());

    type_modifier_build mods[] {
        {derived_type_kind::pointer, 0},
        {derived_type_kind::array, 4}
    };
    member_build members[] {{m, std::nullopt, b, 0, 2}};
    // B is not materialized yet: this is canonical pending, not source unresolved.
    assert(replacement.define_members(at, members, mods).ok());
    assert(replacement.add_named_type(b, aggregate_definition_state::declared, be, bt).ok());
    assert(tx.commit().ok());

    const auto* aentity = gm.compiled_graph().find(gm.strings().find("A"));
    assert(aentity);
    const auto span = gm.compiled_graph().members(aentity->type);
    assert(span.size() == 1);
    const auto* outer = gm.compiled_graph().derived(span[0].type);
    assert(outer && outer->kind == derived_type_kind::array && outer->payload == 4);
    const auto* inner = gm.compiled_graph().derived(outer->child);
    assert(inner && inner->kind == derived_type_kind::pointer);
    type_handle named;
    assert(gm.compiled_graph().named(inner->child, named));
    const auto* bentity = gm.compiled_graph().find(gm.strings().find("B"));
    assert(bentity && named == bentity->type);

    std::cout << "PASS\n";
}
