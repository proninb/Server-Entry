#pragma once
#include <cstdint>
namespace cw::server {
class member_index final {
public:
 constexpr member_index() noexcept=default;
 explicit constexpr member_index(std::uint32_t value) noexcept:value_(value){}
 [[nodiscard]] constexpr std::uint32_t value()const noexcept{return value_;}
 [[nodiscard]] constexpr explicit operator bool()const noexcept{return value_!=0;}
 friend constexpr bool operator==(member_index,member_index)noexcept=default;
private: std::uint32_t value_=0;
};
}
