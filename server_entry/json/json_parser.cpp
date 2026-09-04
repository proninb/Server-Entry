#include "json_parser.hpp"

#include "json_ascii.hpp"
#include "json_unicode.hpp"

#include <charconv>
#include <system_error>

namespace cw::server {

// Parses one complete JSON document and emits semantic events directly to the
// supplied handler, avoiding construction of an intermediate DOM.
bool json_parser::parse(json_event_handler& handler, json_error& error) noexcept {
    error = {};
    position = 0;

    try {
        whitespace();

        if (!parse_value(handler, error)) {
            return false;
        }

        whitespace();

        if (position != input.size()) {
            return fail(error, json_error_code::trailing_characters, position);
        }

        return true;
    }
    catch (...) {
        return fail(error, json_error_code::allocation_failed, position);
    }
}

bool json_parser::parse_value(json_event_handler& handler, json_error& error) {
    whitespace();

    if (position >= input.size()) {
        return fail(error, json_error_code::unexpected_end, position);
    }

    handler.location(position);

    switch (input[position]) {
    case '{':
        return parse_object(handler, error);

    case '[':
        return parse_array(handler, error);

    case '"':
        if (!parse_string(string_buffer, error)) {
            return false;
        }

        handler.string(string_buffer.view());
        return true;

    case 't':
        if (!consume_literal("true", error)) {
            return false;
        }

        handler.boolean(true);
        return true;

    case 'f':
        if (!consume_literal("false", error)) {
            return false;
        }

        handler.boolean(false);
        return true;

    case 'n':
        if (!consume_literal("null", error)) {
            return false;
        }

        handler.null();
        return true;

    default:
        if (input[position] == '-' || json_is_digit(input[position])) {
            return parse_number(handler, error);
        }

        return fail(error, json_error_code::unexpected_token, position);
    }
}

bool json_parser::parse_object(json_event_handler& handler, json_error& error) {
    ++position;
    handler.object_begin();
    whitespace();

    if (position < input.size() && input[position] == '}') {
        handler.location(position);
        ++position;
        handler.object_end();
        return true;
    }

    while (true) {
        whitespace();

        if (position >= input.size()) {
            return fail(error, json_error_code::unexpected_end, position);
        }

        if (input[position] != '"') {
            return fail(error, json_error_code::expected_object_key, position);
        }

        const auto key_offset = position;

        if (!parse_string(string_buffer, error)) {
            return false;
        }

        handler.location(key_offset);
        handler.key(string_buffer.view());

        whitespace();

        if (position >= input.size()) {
            return fail(error, json_error_code::unexpected_end, position);
        }

        if (input[position] != ':') {
            return fail(error, json_error_code::expected_colon, position);
        }

        ++position;

        if (!parse_value(handler, error)) {
            return false;
        }

        whitespace();

        if (position >= input.size()) {
            return fail(error, json_error_code::unexpected_end, position);
        }

        if (input[position] == '}') {
            handler.location(position);
            ++position;
            handler.object_end();
            return true;
        }

        if (input[position] != ',') {
            return fail(error, json_error_code::expected_comma_or_end, position);
        }

        ++position;
    }
}

bool json_parser::parse_array(json_event_handler& handler, json_error& error) {
    ++position;
    handler.array_begin();
    whitespace();

    if (position < input.size() && input[position] == ']') {
        handler.location(position);
        ++position;
        handler.array_end();
        return true;
    }

    while (true) {
        if (!parse_value(handler, error)) {
            return false;
        }

        whitespace();

        if (position >= input.size()) {
            return fail(error, json_error_code::unexpected_end, position);
        }

        if (input[position] == ']') {
            handler.location(position);
            ++position;
            handler.array_end();
            return true;
        }

        if (input[position] != ',') {
            return fail(error, json_error_code::expected_comma_or_end, position);
        }

        ++position;
    }
}

// Decodes one JSON string into the reusable string buffer.
// Raw UTF-8 segments are validated before being copied, and \u escapes are
// normalized to UTF-8 before the string event is emitted.
bool json_parser::parse_string(json_buffer& output, json_error& error) {
    const auto start = position++;
    output.clear();

    std::size_t raw_start = position;

    while (position < input.size()) {
        const auto character = static_cast<unsigned char>(input[position]);

        if (character == '"') {
            if (!json_valid_utf8(input.substr(raw_start, position - raw_start))) {
                return fail(error, json_error_code::invalid_unicode, raw_start);
            }

            if (!output.append(input.substr(raw_start, position - raw_start))) {
                return fail(error, json_error_code::allocation_failed, position);
            }

            ++position;
            return true;
        }

        if (character < 0x20) {
            return fail(error, json_error_code::invalid_string, position);
        }

        if (character != '\\') {
            ++position;
            continue;
        }

        if (!json_valid_utf8(input.substr(raw_start, position - raw_start))) {
            return fail(error, json_error_code::invalid_unicode, raw_start);
        }

        if (!output.append(input.substr(raw_start, position - raw_start))) {
            return fail(error, json_error_code::allocation_failed, position);
        }

        ++position;

        if (position >= input.size()) {
            return fail(error, json_error_code::unexpected_end, position);
        }

        const char escape = input[position++];

        switch (escape) {
        case '"':
        case '\\':
        case '/':
            if (!output.append(escape)) {
                return fail(error, json_error_code::allocation_failed, position);
            }
            break;

        case 'b':
            if (!output.append('\b')) {
                return fail(error, json_error_code::allocation_failed, position);
            }
            break;

        case 'f':
            if (!output.append('\f')) {
                return fail(error, json_error_code::allocation_failed, position);
            }
            break;

        case 'n':
            if (!output.append('\n')) {
                return fail(error, json_error_code::allocation_failed, position);
            }
            break;

        case 'r':
            if (!output.append('\r')) {
                return fail(error, json_error_code::allocation_failed, position);
            }
            break;

        case 't':
            if (!output.append('\t')) {
                return fail(error, json_error_code::allocation_failed, position);
            }
            break;

        case 'u': {
            const auto unicode_offset = position - 2;

            if (position + 4 > input.size()) {
                return fail(error, json_error_code::invalid_unicode, unicode_offset);
            }

            std::uint32_t code = 0;

            for (int digit = 0; digit < 4; ++digit) {
                const int value = json_hex_value(input[position++]);

                if (value < 0) {
                    return fail(error, json_error_code::invalid_unicode, position - 1);
                }

                code = (code << 4) | static_cast<std::uint32_t>(value);
            }

            if (code >= 0xd800 && code <= 0xdbff) {
                if (position + 6 > input.size() ||
                    input[position] != '\\' ||
                    input[position + 1] != 'u') {
                    return fail(error, json_error_code::invalid_unicode, unicode_offset);
                }

                position += 2;

                std::uint32_t low = 0;

                for (int digit = 0; digit < 4; ++digit) {
                    const int value = json_hex_value(input[position++]);

                    if (value < 0) {
                        return fail(error, json_error_code::invalid_unicode, position - 1);
                    }

                    low = (low << 4) | static_cast<std::uint32_t>(value);
                }

                if (low < 0xdc00 || low > 0xdfff) {
                    return fail(error, json_error_code::invalid_unicode, unicode_offset);
                }

                code = 0x10000 +
                       ((code - 0xd800) << 10) +
                       (low - 0xdc00);
            }
            else if (code >= 0xdc00 && code <= 0xdfff) {
                return fail(error, json_error_code::invalid_unicode, unicode_offset);
            }

            if (!json_append_utf8(output, code)) {
                return fail(error, json_error_code::allocation_failed, unicode_offset);
            }

            break;
        }

        default:
            return fail(error, json_error_code::invalid_escape, position - 1);
        }

        raw_start = position;
    }

    return fail(error, json_error_code::unexpected_end, start);
}

// Parses the JSON number grammar first, then converts the exact source range
// directly to either int64 or double before emitting the corresponding event.
bool json_parser::parse_number(json_event_handler& handler, json_error& error) {
    const auto start = position;

    if (input[position] == '-') {
        ++position;
    }

    if (position >= input.size()) {
        return fail(error, json_error_code::invalid_number, start);
    }

    if (input[position] == '0') {
        ++position;

        if (position < input.size() && json_is_digit(input[position])) {
            return fail(error, json_error_code::invalid_number, position);
        }
    }
    else {
        if (input[position] < '1' || input[position] > '9') {
            return fail(error, json_error_code::invalid_number, position);
        }

        while (position < input.size() && json_is_digit(input[position])) {
            ++position;
        }
    }

    bool floating = false;

    if (position < input.size() && input[position] == '.') {
        floating = true;
        ++position;

        if (position >= input.size() || !json_is_digit(input[position])) {
            return fail(error, json_error_code::invalid_number, position);
        }

        while (position < input.size() && json_is_digit(input[position])) {
            ++position;
        }
    }

    if (position < input.size() &&
        (input[position] == 'e' || input[position] == 'E')) {
        floating = true;
        ++position;

        if (position < input.size() &&
            (input[position] == '+' || input[position] == '-')) {
            ++position;
        }

        if (position >= input.size() || !json_is_digit(input[position])) {
            return fail(error, json_error_code::invalid_number, position);
        }

        while (position < input.size() && json_is_digit(input[position])) {
            ++position;
        }
    }

    const auto text = input.substr(start, position - start);

    if (floating) {
        double value = 0;
        const auto result = std::from_chars(
            text.data(),
            text.data() + text.size(),
            value,
            std::chars_format::general);

        if (result.ec != std::errc{} ||
            result.ptr != text.data() + text.size()) {
            return fail(error, json_error_code::invalid_number, start);
        }

        handler.number(value);
    }
    else {
        std::int64_t value = 0;
        const auto result = std::from_chars(
            text.data(),
            text.data() + text.size(),
            value);

        if (result.ec != std::errc{} ||
            result.ptr != text.data() + text.size()) {
            return fail(error, json_error_code::invalid_number, start);
        }

        handler.integer(value);
    }

    return true;
}

bool json_parser::consume_literal(std::string_view literal, json_error& error) {
    if (input.substr(position, literal.size()) != literal) {
        return fail(error, json_error_code::unexpected_token, position);
    }

    position += literal.size();
    return true;
}

// Centralizes parse failure reporting so every parser path records one error
// category together with the byte offset in the original JSON input.
bool json_parser::fail(
    json_error& error,
    json_error_code code,
    std::size_t offset) noexcept {

    error = {code, offset};
    return false;
}

void json_parser::whitespace() noexcept {
    while (position < input.size() && json_is_whitespace(input[position])) {
        ++position;
    }
}

} // namespace cw::server
