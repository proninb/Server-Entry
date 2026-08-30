#pragma once

#include "json_buffer.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace cw::server
{

class json_writer
{
public:
    explicit json_writer(json_buffer& output) noexcept;

    bool begin_object() noexcept;
    bool end_object() noexcept;
    bool begin_array() noexcept;
    bool end_array() noexcept;
    bool key(std::string_view name) noexcept;
    bool string(std::string_view value) noexcept;
    bool integer(std::int64_t value) noexcept;
    bool number(double value) noexcept;
    bool boolean(bool value) noexcept;
    bool null() noexcept;

    [[nodiscard]] bool complete() const noexcept;

private:
    enum class container { object, array };
    struct frame { container type; bool first = true; bool awaiting_value = false; };

    bool before_value() noexcept;
    bool quoted(std::string_view value) noexcept;

    json_buffer& output_;
    std::vector<frame> stack_;
    bool root_written_ = false;
    bool valid_ = true;
};

} // namespace cw::server
