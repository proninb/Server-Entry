#pragma once
#include "../../source_id.hpp"
#include "../graph/builtin_type.hpp"
#include "../language/enum_semantics.hpp"
#include "../language/aggregate_semantics.hpp"
#include <cstdint>
#include <optional>
#include <span>
namespace cw::server {
struct source_text_range { std::uint32_t offset=0,length=0; };
struct source_name_ref { std::uint32_t offset=0,length=0; explicit constexpr operator bool()const noexcept{return length!=0;} };
struct enum_value_source_fact { source_name_ref name{}; integral_constant value{}; source_text_range name_range{},expression_range{}; };
struct enum_declaration_source_fact {
 source_name_ref canonical_name{},scope_name{}; bool anonymous=false,scoped=false;
 enum_definition_state definition_state=enum_definition_state::defined;
 std::optional<builtin_type> explicit_underlying;
 std::uint32_t enumerator_offset=0,enumerator_count=0;
 source_text_range declaration_range{},name_range{},underlying_range{};
};
struct aggregate_declaration_source_fact {
 source_name_ref canonical_name{},scope_name{};
 aggregate_definition_state definition_state=aggregate_definition_state::declared;
 std::uint32_t member_offset=0,member_count=0;
 source_text_range declaration_range{},name_range{};
};
struct member_declaration_source_fact { source_name_ref name{},type_name{}; std::optional<builtin_type> builtin; bool lvalue_reference=false; source_text_range declaration_range{},name_range{},type_range{}; };
class source_context;
struct parser_source_fact_batch { source_id source{}; const source_context* context=nullptr; std::span<const enum_declaration_source_fact> enums; std::span<const aggregate_declaration_source_fact> aggregates; };
}
