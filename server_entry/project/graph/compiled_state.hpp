#pragma once

#include "graph.hpp"

#include <string>
#include <vector>

namespace cw::server
{
struct compiled_entity_slot { bool live=false; entity_kind kind=entity_kind::enum_type; std::uint32_t id=0,name=0,defining_source=0,type=0; };
struct compiled_enum_value { std::uint32_t name=0; std::uint64_t bits=0; };
struct compiled_type_slot
{
    bool live=false;
    user_type_record record{};
    bool anonymous=false;
    source_id anonymous_source{};
    std::vector<compiled_enum_value> enumerators;
    std::uint32_t member_begin=0, member_count=0;
};
struct compiled_member { std::uint32_t name=0, type_ref=0; };
struct compiled_canonical_type { std::uint8_t kind=0, builtin=0; std::uint32_t named=0, child=0; std::uint64_t payload=0; };
struct compiled_graph_state
{
    abi_configuration abi{};
    std::vector<std::uint32_t> identities;
    std::vector<compiled_entity_slot> entities;
    std::vector<compiled_type_slot> types;
    std::vector<compiled_member> members;
    std::vector<compiled_canonical_type> canonical_types;
};
struct compiled_project_state
{
    std::vector<std::string> strings;
    compiled_graph_state graph;
};
}
