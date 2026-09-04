#include "json_writer.hpp"

#include "json_escape.hpp"
#include "json_unicode.hpp"

#include <charconv>
#include <cmath>

namespace cw::server {

json_writer::json_writer(json_buffer& output) noexcept : output(output) {
    output.clear();
}

bool json_writer::before_value() noexcept {
    if (!valid) {
        return false;
    }

    if (stack.empty()) {
        if (root_written) {
            return valid = false;
        }

        root_written = true;
        return true;
    }

    auto& frame = stack.back();

    if (frame.type == container::object) {
        if (!frame.awaiting_value) {
            return valid = false;
        }

        frame.awaiting_value = false;
        return true;
    }

    if (!frame.first && !output.append(',')) {
        return valid = false;
    }

    frame.first = false;
    return true;
}

bool json_writer::begin_object() noexcept {
    if (!before_value() || !output.append('{')) {
        return valid = false;
    }

    try {
        stack.push_back({container::object});
        return true;
    }
    catch (...) {
        return valid = false;
    }
}

bool json_writer::end_object() noexcept {
    if (!valid ||
        stack.empty() ||
        stack.back().type != container::object ||
        stack.back().awaiting_value) {
        return valid = false;
    }

    stack.pop_back();
    return output.append('}') || (valid = false);
}

bool json_writer::begin_array() noexcept {
    if (!before_value() || !output.append('[')) {
        return valid = false;
    }

    try {
        stack.push_back({container::array});
        return true;
    }
    catch (...) {
        return valid = false;
    }
}

bool json_writer::end_array() noexcept {
    if (!valid ||
        stack.empty() ||
        stack.back().type != container::array) {
        return valid = false;
    }

    stack.pop_back();
    return output.append(']') || (valid = false);
}

bool json_writer::quoted(std::string_view value) noexcept {
    if (!json_valid_utf8(value) ||
        !output.append('"') ||
        !json_append_escaped(output, value) ||
        !output.append('"')) {
        return valid = false;
    }

    return true;
}

bool json_writer::key(std::string_view name) noexcept {
    if (!valid ||
        stack.empty() ||
        stack.back().type != container::object ||
        stack.back().awaiting_value) {
        return valid = false;
    }

    auto& frame = stack.back();

    if (!frame.first && !output.append(',')) {
        return valid = false;
    }

    frame.first = false;

    if (!quoted(name) || !output.append(':')) {
        return valid = false;
    }

    frame.awaiting_value = true;
    return true;
}

bool json_writer::string(std::string_view value) noexcept {
    return before_value() && quoted(value);
}

bool json_writer::integer(std::int64_t value) noexcept {
    return before_value() && output.append_integer(value);
}

bool json_writer::number(double value) noexcept {
    if (!before_value() || !std::isfinite(value)) {
        return valid = false;
    }

    char storage[64];
    const auto result = std::to_chars(
        storage,
        storage + sizeof(storage),
        value,
        std::chars_format::general);

    if (result.ec != std::errc{} ||
        !output.append({storage, result.ptr})) {
        return valid = false;
    }

    return true;
}

bool json_writer::boolean(bool value) noexcept {
    return before_value() && output.append(value ? "true" : "false");
}

bool json_writer::null() noexcept {
    return before_value() && output.append("null");
}

bool json_writer::complete() const noexcept {
    return valid && root_written && stack.empty();
}

} // namespace cw::server
