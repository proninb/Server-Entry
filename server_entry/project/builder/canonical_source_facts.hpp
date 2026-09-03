#pragma once

#include "../../source_id.hpp"
#include "../../string_id.hpp"
#include "../graph/builtin_type.hpp"
#include "../language/enum_semantics.hpp"
#include "../language/aggregate_semantics.hpp"

#include <optional>
#include <span>

namespace cw::server
{

// Builder-ready facts contain canonical string identities. Their order is
// authoritative and is established before project_builder is entered.
struct enum_value_fact
{
    string_id name{};
    integral_constant value{};
};

struct enum_source_fact
{
    string_id canonical_name{};
    bool anonymous = false;
    bool scoped = false;
    enum_definition_state definition_state = enum_definition_state::defined;
    std::optional<builtin_type> explicit_underlying;
    std::span<const enum_value_fact> enumerators;
};
struct aggregate_source_fact
{
    string_id canonical_name{};
    aggregate_definition_state definition_state = aggregate_definition_state::declared;
    struct member_fact { string_id name{}; std::optional<builtin_type> builtin; string_id user_type_name{}; bool lvalue_reference=false; };
    std::span<const member_fact> members{};
};

struct source_fact_batch
{
    source_id source{};
    std::span<const enum_source_fact> enums;
    std::span<const aggregate_source_fact> aggregates;
};

} // namespace cw::server
