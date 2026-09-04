#include "project/graph/graph_manager.hpp"
#include "project/graph/graph_build_transaction.hpp"
#include <cassert>
#include <iostream>
using namespace cw::server;
int main() {
    graph_manager manager;
    abi_configuration abi{abi_target::posix_x64, 8};
    assert(manager.initialize(abi).ok());
    auto tx = manager.begin_build();
    assert(tx.sources().add("bad.cpp", project_item_role::source).ok());
    string_id name, value_name;
    assert(tx.strings().intern("E", name).ok());
    assert(tx.strings().intern("V", value_name).ok());
    graph_update::source_replacement replacement;
    assert(tx.graph_state().replace_source(tx.sources().roots()[0].source, replacement).ok());
    enum_value_build value{value_name, {builtin_type::long_double_floating, 0}};
    enum_build_data data{enum_definition_state::defined, true, std::nullopt,
                         std::span<const enum_value_build>{&value, 1}};
    stable_id entity;
    type_handle type;
    const auto result = replacement.add_named_enum(name, data, entity, type);
    assert(!result.ok());
    assert(result.code == status_code::configuration_failed);
    std::cout << "PASS\n";
}
