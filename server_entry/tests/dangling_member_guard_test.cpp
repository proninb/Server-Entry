#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"

#include <cassert>
#include <iostream>

using namespace cw::server;

int main() {
    graph_manager manager;
    abi_configuration abi{abi_target::posix_x64, 8};
    assert(manager.initialize(abi).ok());

    source_id source_b;
    source_id source_a;
    {
        auto tx = manager.begin_build();
        assert(tx.sources().add("b.cpp", project_item_role::source).ok());
        assert(tx.sources().add("a.cpp", project_item_role::source).ok());
        const auto roots = tx.sources().roots();
        source_b = roots[0].source;
        source_a = roots[1].source;

        string_id A, B, member_name;
        assert(tx.strings().intern("A", A).ok());
        assert(tx.strings().intern("B", B).ok());
        assert(tx.strings().intern("b", member_name).ok());

        graph_update::source_replacement rb;
        assert(tx.graph_state().replace_source(source_b, rb).ok());
        stable_id be;
        type_handle bt;
        assert(rb.add_named_type(B, aggregate_definition_state::declared, be, bt).ok());

        graph_update::source_replacement ra;
        assert(tx.graph_state().replace_source(source_a, ra).ok());
        stable_id ae;
        type_handle at;
        assert(ra.add_named_type(A, aggregate_definition_state::defined, ae, at).ok());
        member_build member{member_name, std::nullopt, B, 0, 0};
        assert(ra.define_members(at, std::span<const member_build>{&member, 1}, {}).ok());

        assert(tx.commit().ok());
    }

    const auto a_name = manager.strings().find("A");
    const auto b_name = manager.strings().find("B");
    assert(a_name && b_name);
    assert(manager.compiled_graph().find(a_name));
    assert(manager.compiled_graph().find(b_name));

    // Replace only B's Source contribution and remove B. A remains live and still
    // refers to B, so the Graph candidate must be rejected before publication.
    {
        auto tx = manager.begin_build(graph_build_mode::incremental);
        assert(tx.sources().add("b.cpp", project_item_role::source).ok());
        assert(tx.sources().add("a.cpp", project_item_role::source).ok());

        graph_update::source_replacement rb;
        assert(tx.graph_state().replace_source(source_b, rb).ok());

        const auto result = tx.commit();
        assert(!result.ok());
        assert(result.code == status_code::configuration_failed);
        assert(manager.state() == project_state::error);
    }

    std::cout << "PASS\n";
}
