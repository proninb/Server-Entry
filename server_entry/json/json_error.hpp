#pragma once

#include <cstddef>
#include <cstdint>

namespace cw::server {

// Defines the syntax and resource failures that can terminate JSON parsing.
// The codes describe parser-level failures only and are independent of any
// configuration or application schema built on top of the JSON parser.
enum class json_error_code : std::uint16_t {
    none = 0,
    unexpected_end,
    unexpected_token,
    expected_object_key,
    expected_colon,
    expected_comma_or_end,
    invalid_string,
    invalid_escape,
    invalid_unicode,
    invalid_number,
    trailing_characters,
    allocation_failed,
};

// Represents one JSON parser failure.
// The byte offset identifies where parsing failed in the original input text.
struct json_error {
    json_error_code code = json_error_code::none;
    std::size_t offset = 0;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return code == json_error_code::none;
    }
};

} // namespace cw::server
