#include "project/graph/compiled_state.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include "project/graph/graph_manager.hpp"

#include <cassert>
#include <iostream>

using namespace cw::server;

template <class T>
concept has_redundant_id = requires(T value) {
    value.id;
};

template <class T>
concept has_hot_source_provenance = requires(T value) {
    value.defining_source;
};


template <class T>
concept has_type_build_members = requires(T value) {
    value.pending_members;
    value.pending_modifiers;
    value.definition_pending;
};

template <class T>
concept has_anonymous_provenance = requires(T value) {
    value.anonymous;
    value.anonymous_source;
};

static_assert(!has_redundant_id<entity_entry>);
static_assert(!has_hot_source_provenance<entity_entry>);
static_assert(!has_type_build_members<type_entry>);
static_assert(!has_anonymous_provenance<compiled_type_slot>);

static const type_entry& type_of(const graph& state, string_id name) {
    const auto* entity = state.find(name);
    assert(entity && entity->type);
    const auto* type = state.find(entity->type);
    assert(type);
    return *type;
}

int main() {
    graph_manager manager;
    const abi_configuration abi{abi_target::posix_x64, 8};
    assert(manager.initialize(abi).ok());

    auto transaction = manager.begin_build();
    assert(transaction.sources().add("types.cpp", project_item_role::source).ok());
    const auto source = transaction.sources().roots()[0].source;

    string_id A, B, E, F;
    assert(transaction.strings().intern("A", A).ok());
    assert(transaction.strings().intern("B", B).ok());
    assert(transaction.strings().intern("E", E).ok());
    assert(transaction.strings().intern("F", F).ok());

    graph_update::source_replacement replacement;
    assert(transaction.graph_state().replace_source(source, replacement).ok());

    stable_id entity;
    type_handle type;

    // Incomplete aggregate: no canonical definition range.
    assert(replacement.add_named_type(
        A, aggregate_definition_state::declared, entity, type).ok());

    // Defined but empty aggregate: valid one-based range with count == 0.
    assert(replacement.add_named_type(
        B, aggregate_definition_state::defined, entity, type).ok());
    assert(replacement.define_members(type, {}, {}).ok());

    // Opaque enum: no definition range. Explicit underlying type makes this a
    // valid opaque enum semantic input for this Graph-level test.
    enum_build_data opaque;
    opaque.definition_state = enum_definition_state::opaque;
    opaque.scoped = true;
    opaque.explicit_underlying = builtin_type::integer;
    assert(replacement.add_named_enum(E, opaque, entity, type).ok());

    // Defined but empty enum: valid one-based range with count == 0.
    enum_build_data defined;
    defined.definition_state = enum_definition_state::defined;
    defined.scoped = true;
    defined.explicit_underlying = builtin_type::integer;
    assert(replacement.add_named_enum(F, defined, entity, type).ok());

    assert(transaction.commit().ok());

    const auto& graph_state = manager.compiled_graph();
    const auto& a = type_of(graph_state, A);
    const auto& b = type_of(graph_state, B);
    const auto& e = type_of(graph_state, E);
    const auto& f = type_of(graph_state, F);

    assert(a.kind == user_type_kind::aggregate && !a.definition);
    assert(b.kind == user_type_kind::aggregate && b.definition);
    assert(b.definition.begin != 0 && b.definition.count == 0);
    assert(graph_state.members(graph_state.find(B)->type).empty());

    assert(e.kind == user_type_kind::enumeration && !e.definition);
    assert(f.kind == user_type_kind::enumeration && f.definition);
    assert(f.definition.begin != 0 && f.definition.count == 0);
    assert(graph_state.enum_values(graph_state.find(F)->type).empty());

    compiled_graph_state compiled;
    assert(graph_state.export_compiled(compiled).ok());

    const auto b_handle = graph_state.find(B)->type.value();
    const auto f_handle = graph_state.find(F)->type.value();
    assert(compiled.types[b_handle - 1].record.definition.begin != 0);
    assert(compiled.types[b_handle - 1].record.definition.count == 0);
    assert(compiled.types[f_handle - 1].record.definition.begin != 0);
    assert(compiled.types[f_handle - 1].record.definition.count == 0);

    graph imported;
    assert(imported.initialize(abi).ok());
    assert(imported.import_compiled(compiled).ok());
    assert(type_of(imported, A).definition.valid() == false);
    assert(type_of(imported, B).definition.valid());
    assert(type_of(imported, E).definition.valid() == false);
    assert(type_of(imported, F).definition.valid());

    // begin == 0 with non-zero count is structurally impossible in v2.
    auto corrupt = compiled;
    corrupt.types[b_handle - 1].record.definition = {0, 1};
    graph bad_zero;
    assert(bad_zero.initialize(abi).ok());
    assert(bad_zero.import_compiled(corrupt).code == status_code::artifact_corrupt);

    // A one-based range must still stay inside its selected definition arena.
    corrupt = compiled;
    corrupt.types[b_handle - 1].record.definition = {0xfffffff0u, 0};
    graph bad_range;
    assert(bad_range.initialize(abi).ok());
    assert(bad_range.import_compiled(corrupt).code == status_code::artifact_corrupt);

    std::cout << "PASS\n";
}
