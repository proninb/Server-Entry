#pragma once

#include <charconv>
#include <string>
#include <string_view>

namespace cw::server
{

class json_buffer
{
public:
    void clear() noexcept { value_.clear(); }
    void reserve(std::size_t count) { value_.reserve(count); }

    [[nodiscard]] bool append(char value) noexcept
    {
        try { value_.push_back(value); return true; } catch (...) { return false; }
    }

    [[nodiscard]] bool append(std::string_view value) noexcept
    {
        try { value_.append(value); return true; } catch (...) { return false; }
    }

    template <typename Integer>
    [[nodiscard]] bool append_integer(Integer value) noexcept
    {
        char storage[32];
        const auto result = std::to_chars(storage, storage + sizeof(storage), value);
        return result.ec == std::errc{} && append({storage, result.ptr});
    }

    [[nodiscard]] std::string_view view() const noexcept { return value_; }
    [[nodiscard]] const std::string& string() const noexcept { return value_; }

private:
    std::string value_;
};

} // namespace cw::server
