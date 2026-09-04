#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"

#include <cassert>
#include <iostream>

using namespace cw::server;

namespace {

void add_defined_type(
    graph_build_transaction& tx,
    source_id source,
    string_id name,
    stable_id& entity,
    type_handle& type) {

    graph_update::source_replacement replacement;
    assert(tx.graph_state().replace_source(source, replacement).ok());
    assert(replacement.add_named_type(
        name,
        aggregate_definition_state::defined,
        entity,
        type).ok());
    assert(replacement.define_members(type, {}, {}).ok());
}

} // namespace

int main() {
    graph_manager manager;
    assert(manager.initialize({abi_target::posix_x64, 8}).ok());

    source_id source_a;
    source_id source_b;
    string_id name_a;
    string_id name_b;
    string_id name_c;

    stable_id a_id;
    stable_id b_id;
    stable_id c_id;

    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);
        assert(tx.sources().add("a.cpp", project_item_role::source).ok());
        assert(tx.sources().add("b.cpp", project_item_role::source).ok());
        const auto roots = tx.sources().roots();
        assert(roots.size() == 2);
        source_a = roots[0].source;
        source_b = roots[1].source;

        assert(tx.strings().intern("A", name_a).ok());
        assert(tx.strings().intern("B", name_b).ok());

        type_handle a_type;
        type_handle b_type;
        add_defined_type(tx, source_a, name_a, a_id, a_type);
        add_defined_type(tx, source_b, name_b, b_id, b_type);
        assert(tx.commit().ok());
    }

    assert(manager.compiled_graph().entity_count() == 2);
    assert(manager.compiled_graph().user_type_count() == 2);
    assert(manager.compiled_graph().find(a_id));
    assert(manager.compiled_graph().find(b_id));

    // G0 -> G1: remove B. This creates historical Entity/type-slot state that
    // the next rebuild must not copy as current physical Graph storage.
    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        graph_update::source_replacement replacement;
        assert(tx.graph_state().replace_source(source_b, replacement).ok());
        assert(tx.commit().ok());
    }

    assert(manager.compiled_graph().find(a_id));
    assert(!manager.compiled_graph().find(b_id));
    assert(manager.compiled_graph().entity_count() == 1);

    // G1 -> G2: add C through B's Source. Incremental allocation may reuse an
    // available physical type slot; persistent Entity identity is independent.
    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        assert(tx.strings().intern("C", name_c).ok());
        type_handle c_type;
        add_defined_type(tx, source_b, name_c, c_id, c_type);
        assert(tx.commit().ok());
    }

    assert(manager.compiled_graph().find(a_id));
    assert(manager.compiled_graph().find(c_id));
    assert(!manager.compiled_graph().find(b_id));

    const auto stable_a = a_id;
    const auto stable_c = c_id;

    // Explicit Rebuild constructs detached G0 from the complete Source universe.
    // It preserves Project stable IDs but creates a fresh dense type namespace.
    {
        auto tx = manager.begin_build(graph_build_mode::rebuild);

        assert(tx.strings().intern("C", name_c).ok());
        stable_id rebuilt_a;
        stable_id rebuilt_c;
        type_handle a_type;
        type_handle c_type;

        add_defined_type(tx, source_a, name_a, rebuilt_a, a_type);
        add_defined_type(tx, source_b, name_c, rebuilt_c, c_type);
        assert(tx.commit().ok());

        assert(rebuilt_a == stable_a);
        assert(rebuilt_c == stable_c);
    }

    const auto& graph = manager.compiled_graph();
    const auto* a = graph.find(stable_a);
    const auto* c = graph.find(stable_c);

    assert(a);
    assert(c);
    assert(!graph.find(b_id));
    assert(graph.entity_count() == 2);
    assert(graph.user_type_count() == 2);

    // G0's generation-local type slots are reconstructed densely from current
    // live definitions and do not retain historical holes or slot provenance.
    assert(a->type);
    assert(c->type);
    assert(a->type != c->type);
    assert(a->type.value() <= 2);
    assert(c->type.value() <= 2);

    std::cout << "PASS\n";
}
