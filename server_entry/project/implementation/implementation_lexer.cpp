#include "implementation_lexer.hpp"

#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <cctype>
#include <limits>

namespace cw::server {
namespace {

bool identifier_begin(unsigned char value) noexcept {
    return value == '_' ||
        std::isalpha(value) != 0;
}

bool identifier_continue(unsigned char value) noexcept {
    return identifier_begin(value) ||
        std::isdigit(value) != 0;
}

bool valid_escape(char value) noexcept {
    switch (value) {
    case '0':
    case 'n':
    case 'r':
    case 't':
    case '\\':
    case '\'':
    case '"':
        return true;

    default:
        return false;
    }
}

status emit_invalid(
    source_view source,
    operation_id operation,
    diagnostic_buffer& buffer,
    std::uint32_t offset,
    std::uint32_t length) noexcept {

    try {
        buffer.emit({
            diagnostics::implementation_invalid_source.id,
            diagnostics::implementation_invalid_source.default_severity,
            operation,
            {source.source, offset, length},
            {}
        });

        return {
            status_code::configuration_failed
        };
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

} // namespace

status lex_implementation_source(
    source_view source,
    operation_id operation,
    diagnostic_buffer& buffer,
    std::vector<implementation_token>& output) noexcept {

    output.clear();

    if (!source.source ||
        source.bytes.size() >
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {
            status_code::configuration_failed
        };
    }

    try {
        const auto size =
            static_cast<std::uint32_t>(
                source.bytes.size());

        std::uint32_t position = 0;

        while (position < size) {
            const auto value =
                static_cast<unsigned char>(
                    source.bytes[position]);

            if (std::isspace(value) != 0) {
                ++position;
                continue;
            }

            if (source.bytes[position] == '/' &&
                position + 1 < size) {
                const auto next =
                    source.bytes[position + 1];

                if (next == '/') {
                    position += 2;

                    while (position < size &&
                           source.bytes[position] != '\n') {
                        ++position;
                    }

                    continue;
                }

                if (next == '*') {
                    const auto begin = position;
                    position += 2;
                    bool closed = false;

                    while (position + 1 < size) {
                        if (source.bytes[position] == '*' &&
                            source.bytes[position + 1] == '/') {
                            position += 2;
                            closed = true;
                            break;
                        }

                        ++position;
                    }

                    if (!closed) {
                        return emit_invalid(
                            source,
                            operation,
                            buffer,
                            begin,
                            size - begin);
                    }

                    continue;
                }
            }

            if (identifier_begin(value)) {
                const auto begin = position++;
                while (position < size &&
                       identifier_continue(
                           static_cast<unsigned char>(
                               source.bytes[position]))) {
                    ++position;
                }

                output.push_back({
                    implementation_token_kind::identifier,
                    implementation_punctuation::semicolon,
                    begin,
                    position - begin
                });

                continue;
            }

            if (std::isdigit(value) != 0) {
                const auto begin = position++;

                while (position < size &&
                       std::isdigit(
                           static_cast<unsigned char>(
                               source.bytes[position])) != 0) {
                    ++position;
                }

                output.push_back({
                    implementation_token_kind::integer_literal,
                    implementation_punctuation::semicolon,
                    begin,
                    position - begin
                });

                continue;
            }

            if (source.bytes[position] == '\'' ||
                source.bytes[position] == '"') {
                const auto quote =
                    source.bytes[position];
                const auto begin = position++;
                bool closed = false;

                while (position < size) {
                    const auto current =
                        source.bytes[position];

                    if (current == '\n' ||
                        current == '\r') {
                        break;
                    }

                    if (current == quote) {
                        ++position;
                        closed = true;
                        break;
                    }

                    if (current == '\\') {
                        if (position + 1 >= size ||
                            !valid_escape(
                                source.bytes[position + 1])) {
                            return emit_invalid(
                                source,
                                operation,
                                buffer,
                                position,
                                position + 1 < size ? 2u : 1u);
                        }

                        position += 2;
                        continue;
                    }

                    ++position;
                }

                if (!closed) {
                    return emit_invalid(
                        source,
                        operation,
                        buffer,
                        begin,
                        position > begin
                            ? position - begin
                            : 1);
                }

                output.push_back({
                    quote == '\''
                        ? implementation_token_kind::character_literal
                        : implementation_token_kind::string_literal,
                    implementation_punctuation::semicolon,
                    begin,
                    position - begin
                });

                continue;
            }

            implementation_punctuation punctuation;

            switch (source.bytes[position]) {
            case ';':
                punctuation =
                    implementation_punctuation::semicolon;
                break;
            case ',':
                punctuation =
                    implementation_punctuation::comma;
                break;
            case '=':
                punctuation =
                    implementation_punctuation::equal;
                break;
            case '.':
                punctuation =
                    implementation_punctuation::dot;
                break;
            case ':':
                punctuation =
                    implementation_punctuation::colon;
                break;
            case '[':
                punctuation =
                    implementation_punctuation::left_bracket;
                break;
            case ']':
                punctuation =
                    implementation_punctuation::right_bracket;
                break;

            default:
                return emit_invalid(
                    source,
                    operation,
                    buffer,
                    position,
                    1);
            }

            output.push_back({
                implementation_token_kind::punctuation,
                punctuation,
                position,
                1
            });

            ++position;
        }

        output.push_back({
            implementation_token_kind::eof,
            implementation_punctuation::semicolon,
            size,
            0
        });

        return {};
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

} // namespace cw::server
