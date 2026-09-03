#pragma once
#include <cstdint>
#include <type_traits>

namespace cw::server {
class graph; class graph_update;

class TypeRef final {
public:
 constexpr TypeRef() noexcept=default;
 [[nodiscard]] constexpr std::uint32_t value()const noexcept{return value_;}
 [[nodiscard]] constexpr bool valid()const noexcept{return value_!=0;}
 [[nodiscard]] constexpr explicit operator bool()const noexcept{return valid();}
 friend constexpr bool operator==(TypeRef,TypeRef)noexcept=default;
private:
 explicit constexpr TypeRef(std::uint32_t value)noexcept:value_(value){}
 std::uint32_t value_=0;
 friend class graph;friend class graph_update;
};
static_assert(sizeof(TypeRef)==4);
static_assert(std::is_trivially_copyable_v<TypeRef>);
static_assert(std::is_standard_layout_v<TypeRef>);

enum class canonical_type_kind:std::uint8_t{builtin,named,derived};
enum class derived_type_kind:std::uint8_t{pointer,array,lvalue_reference,rvalue_reference};
struct derived_type_record { derived_type_kind kind=derived_type_kind::pointer; TypeRef child{}; std::uint64_t payload=0; };
static_assert(sizeof(derived_type_record)==16);
}
