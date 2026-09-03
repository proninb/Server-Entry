#pragma once
#include "token.hpp"
#include "source_facts.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include <string_view>
#include <vector>
namespace cw::server {
class source_context final {
public:
 status store_name(std::string_view,source_name_ref&)noexcept;
 status resolve_name(source_name_ref,std::string_view&)const noexcept;
 std::span<const enum_value_source_fact> enumerators(const enum_declaration_source_fact&)const noexcept;
 std::span<const member_declaration_source_fact> members(const aggregate_declaration_source_fact&)const noexcept;
 void reset()noexcept;
 std::vector<parser_token> tokens;
 std::vector<enum_value_source_fact> enum_values;
 std::vector<enum_declaration_source_fact> enums;
 std::vector<aggregate_declaration_source_fact> aggregates;
 std::vector<member_declaration_source_fact> aggregate_members;
 diagnostic_buffer diagnostics;
private: std::vector<char> names_;
}; }
