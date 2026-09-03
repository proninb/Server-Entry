#include "source_context.hpp"
#include <limits>
namespace cw::server {
status source_context::store_name(std::string_view v,source_name_ref& r)noexcept{r={};if(v.empty())return{status_code::configuration_failed};if(v.size()>(std::numeric_limits<std::uint32_t>::max)()||names_.size()>(std::numeric_limits<std::uint32_t>::max)()-v.size())return{status_code::initialization_failed};try{r={static_cast<std::uint32_t>(names_.size()),static_cast<std::uint32_t>(v.size())};names_.insert(names_.end(),v.begin(),v.end());return{};}catch(...){r={};return{status_code::initialization_failed};}}
status source_context::resolve_name(source_name_ref r,std::string_view& out)const noexcept{out={};auto end=std::uint64_t{r.offset}+r.length;if(!r||end>names_.size())return{status_code::configuration_failed};out={names_.data()+r.offset,r.length};return{};}
std::span<const enum_value_source_fact> source_context::enumerators(const enum_declaration_source_fact& f)const noexcept{if(f.enumerator_count==0)return{};auto end=std::uint64_t{f.enumerator_offset}+f.enumerator_count;if(end>enum_values.size())return{};return{enum_values.data()+f.enumerator_offset,f.enumerator_count};}
std::span<const member_declaration_source_fact> source_context::members(const aggregate_declaration_source_fact& f)const noexcept{if(f.member_count==0)return{};auto end=std::uint64_t{f.member_offset}+f.member_count;if(end>aggregate_members.size())return{};return{aggregate_members.data()+f.member_offset,f.member_count};}
void source_context::reset()noexcept{names_.clear();tokens.clear();enum_values.clear();enums.clear();aggregates.clear();aggregate_members.clear();diagnostics.clear();}
}
