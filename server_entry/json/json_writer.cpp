#include "json_writer.hpp"

#include "json_escape.hpp"
#include "json_unicode.hpp"

#include <charconv>
#include <cmath>

namespace cw::server
{

json_writer::json_writer(json_buffer& output) noexcept : output_(output)
{
    output_.clear();
}

bool json_writer::before_value() noexcept
{
    if (!valid_) return false;
    if (stack_.empty())
    {
        if (root_written_) return valid_ = false;
        root_written_ = true;
        return true;
    }
    auto& frame = stack_.back();
    if (frame.type == container::object)
    {
        if (!frame.awaiting_value) return valid_ = false;
        frame.awaiting_value = false;
        return true;
    }
    if (!frame.first && !output_.append(',')) return valid_ = false;
    frame.first = false;
    return true;
}

bool json_writer::begin_object() noexcept
{
    if (!before_value() || !output_.append('{')) return valid_ = false;
    try { stack_.push_back({container::object}); return true; } catch (...) { return valid_ = false; }
}

bool json_writer::end_object() noexcept
{
    if (!valid_ || stack_.empty() || stack_.back().type != container::object || stack_.back().awaiting_value)
        return valid_ = false;
    stack_.pop_back();
    return output_.append('}') || (valid_ = false);
}

bool json_writer::begin_array() noexcept
{
    if (!before_value() || !output_.append('[')) return valid_ = false;
    try { stack_.push_back({container::array}); return true; } catch (...) { return valid_ = false; }
}

bool json_writer::end_array() noexcept
{
    if (!valid_ || stack_.empty() || stack_.back().type != container::array) return valid_ = false;
    stack_.pop_back();
    return output_.append(']') || (valid_ = false);
}

bool json_writer::quoted(std::string_view value) noexcept
{
    if (!json_valid_utf8(value) || !output_.append('"') || !json_append_escaped(output_, value) || !output_.append('"'))
        return valid_ = false;
    return true;
}

bool json_writer::key(std::string_view name) noexcept
{
    if (!valid_ || stack_.empty() || stack_.back().type != container::object || stack_.back().awaiting_value)
        return valid_ = false;
    auto& frame = stack_.back();
    if (!frame.first && !output_.append(',')) return valid_ = false;
    frame.first = false;
    if (!quoted(name) || !output_.append(':')) return valid_ = false;
    frame.awaiting_value = true;
    return true;
}

bool json_writer::string(std::string_view value) noexcept { return before_value() && quoted(value); }
bool json_writer::integer(std::int64_t value) noexcept { return before_value() && output_.append_integer(value); }

bool json_writer::number(double value) noexcept
{
    if (!before_value() || !std::isfinite(value)) return valid_ = false;
    char storage[64];
    const auto result = std::to_chars(storage, storage + sizeof(storage), value, std::chars_format::general);
    if (result.ec != std::errc{} || !output_.append({storage, result.ptr})) return valid_ = false;
    return true;
}

bool json_writer::boolean(bool value) noexcept { return before_value() && output_.append(value ? "true" : "false"); }
bool json_writer::null() noexcept { return before_value() && output_.append("null"); }

bool json_writer::complete() const noexcept
{
    return valid_ && root_written_ && stack_.empty();
}

} // namespace cw::server
