#include "json_parser.hpp"

#include "json_ascii.hpp"
#include "json_unicode.hpp"

#include <charconv>
#include <system_error>

namespace cw::server
{

bool json_parser::parse(json_event_handler& handler, json_error& error) noexcept
{
    error = {};
    position_ = 0;
    try
    {
        whitespace();
        if (!parse_value(handler, error)) return false;
        whitespace();
        if (position_ != input_.size()) return fail(error, json_error_code::trailing_characters, position_);
        return true;
    }
    catch (...)
    {
        return fail(error, json_error_code::allocation_failed, position_);
    }
}

bool json_parser::parse_value(json_event_handler& handler, json_error& error)
{
    whitespace();
    if (position_ >= input_.size()) return fail(error, json_error_code::unexpected_end, position_);
    handler.location(position_);
    switch (input_[position_])
    {
    case '{': return parse_object(handler, error);
    case '[': return parse_array(handler, error);
    case '"':
        if (!parse_string(string_buffer_, error)) return false;
        handler.string(string_buffer_.view());
        return true;
    case 't': if (!consume_literal("true", error)) return false; handler.boolean(true); return true;
    case 'f': if (!consume_literal("false", error)) return false; handler.boolean(false); return true;
    case 'n': if (!consume_literal("null", error)) return false; handler.null(); return true;
    default:
        if (input_[position_] == '-' || json_is_digit(input_[position_])) return parse_number(handler, error);
        return fail(error, json_error_code::unexpected_token, position_);
    }
}

bool json_parser::parse_object(json_event_handler& handler, json_error& error)
{
    ++position_;
    handler.object_begin();
    whitespace();
    if (position_ < input_.size() && input_[position_] == '}')
    {
        handler.location(position_); ++position_; handler.object_end(); return true;
    }
    while (true)
    {
        whitespace();
        if (position_ >= input_.size()) return fail(error, json_error_code::unexpected_end, position_);
        if (input_[position_] != '"') return fail(error, json_error_code::expected_object_key, position_);
        const auto key_offset = position_;
        if (!parse_string(string_buffer_, error)) return false;
        handler.location(key_offset);
        handler.key(string_buffer_.view());
        whitespace();
        if (position_ >= input_.size()) return fail(error, json_error_code::unexpected_end, position_);
        if (input_[position_] != ':') return fail(error, json_error_code::expected_colon, position_);
        ++position_;
        if (!parse_value(handler, error)) return false;
        whitespace();
        if (position_ >= input_.size()) return fail(error, json_error_code::unexpected_end, position_);
        if (input_[position_] == '}') { handler.location(position_); ++position_; handler.object_end(); return true; }
        if (input_[position_] != ',') return fail(error, json_error_code::expected_comma_or_end, position_);
        ++position_;
    }
}

bool json_parser::parse_array(json_event_handler& handler, json_error& error)
{
    ++position_;
    handler.array_begin();
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']')
    {
        handler.location(position_); ++position_; handler.array_end(); return true;
    }
    while (true)
    {
        if (!parse_value(handler, error)) return false;
        whitespace();
        if (position_ >= input_.size()) return fail(error, json_error_code::unexpected_end, position_);
        if (input_[position_] == ']') { handler.location(position_); ++position_; handler.array_end(); return true; }
        if (input_[position_] != ',') return fail(error, json_error_code::expected_comma_or_end, position_);
        ++position_;
    }
}

bool json_parser::parse_string(json_buffer& output, json_error& error)
{
    const auto start = position_++;
    output.clear();
    std::size_t raw_start = position_;
    while (position_ < input_.size())
    {
        const auto character = static_cast<unsigned char>(input_[position_]);
        if (character == '"')
        {
            if (!json_valid_utf8(input_.substr(raw_start, position_ - raw_start)))
                return fail(error, json_error_code::invalid_unicode, raw_start);
            if (!output.append(input_.substr(raw_start, position_ - raw_start)))
                return fail(error, json_error_code::allocation_failed, position_);
            ++position_;
            return true;
        }
        if (character < 0x20) return fail(error, json_error_code::invalid_string, position_);
        if (character != '\\') { ++position_; continue; }

        if (!json_valid_utf8(input_.substr(raw_start, position_ - raw_start)))
            return fail(error, json_error_code::invalid_unicode, raw_start);
        if (!output.append(input_.substr(raw_start, position_ - raw_start)))
            return fail(error, json_error_code::allocation_failed, position_);
        ++position_;
        if (position_ >= input_.size()) return fail(error, json_error_code::unexpected_end, position_);
        const char escape = input_[position_++];
        switch (escape)
        {
        case '"': case '\\': case '/': if (!output.append(escape)) return fail(error, json_error_code::allocation_failed, position_); break;
        case 'b': if (!output.append('\b')) return fail(error, json_error_code::allocation_failed, position_); break;
        case 'f': if (!output.append('\f')) return fail(error, json_error_code::allocation_failed, position_); break;
        case 'n': if (!output.append('\n')) return fail(error, json_error_code::allocation_failed, position_); break;
        case 'r': if (!output.append('\r')) return fail(error, json_error_code::allocation_failed, position_); break;
        case 't': if (!output.append('\t')) return fail(error, json_error_code::allocation_failed, position_); break;
        case 'u':
        {
            const auto unicode_offset = position_ - 2;
            if (position_ + 4 > input_.size()) return fail(error, json_error_code::invalid_unicode, unicode_offset);
            std::uint32_t code = 0;
            for (int digit = 0; digit < 4; ++digit)
            {
                const int value = json_hex_value(input_[position_++]);
                if (value < 0) return fail(error, json_error_code::invalid_unicode, position_ - 1);
                code = (code << 4) | static_cast<std::uint32_t>(value);
            }
            if (code >= 0xd800 && code <= 0xdbff)
            {
                if (position_ + 6 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u')
                    return fail(error, json_error_code::invalid_unicode, unicode_offset);
                position_ += 2;
                std::uint32_t low = 0;
                for (int digit = 0; digit < 4; ++digit)
                {
                    const int value = json_hex_value(input_[position_++]);
                    if (value < 0) return fail(error, json_error_code::invalid_unicode, position_ - 1);
                    low = (low << 4) | static_cast<std::uint32_t>(value);
                }
                if (low < 0xdc00 || low > 0xdfff) return fail(error, json_error_code::invalid_unicode, unicode_offset);
                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            }
            else if (code >= 0xdc00 && code <= 0xdfff)
                return fail(error, json_error_code::invalid_unicode, unicode_offset);
            if (!json_append_utf8(output, code)) return fail(error, json_error_code::allocation_failed, unicode_offset);
            break;
        }
        default: return fail(error, json_error_code::invalid_escape, position_ - 1);
        }
        raw_start = position_;
    }
    return fail(error, json_error_code::unexpected_end, start);
}

bool json_parser::parse_number(json_event_handler& handler, json_error& error)
{
    const auto start = position_;
    if (input_[position_] == '-') ++position_;
    if (position_ >= input_.size()) return fail(error, json_error_code::invalid_number, start);
    if (input_[position_] == '0')
    {
        ++position_;
        if (position_ < input_.size() && json_is_digit(input_[position_]))
            return fail(error, json_error_code::invalid_number, position_);
    }
    else
    {
        if (input_[position_] < '1' || input_[position_] > '9') return fail(error, json_error_code::invalid_number, position_);
        while (position_ < input_.size() && json_is_digit(input_[position_])) ++position_;
    }

    bool floating = false;
    if (position_ < input_.size() && input_[position_] == '.')
    {
        floating = true; ++position_;
        if (position_ >= input_.size() || !json_is_digit(input_[position_])) return fail(error, json_error_code::invalid_number, position_);
        while (position_ < input_.size() && json_is_digit(input_[position_])) ++position_;
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E'))
    {
        floating = true; ++position_;
        if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
        if (position_ >= input_.size() || !json_is_digit(input_[position_])) return fail(error, json_error_code::invalid_number, position_);
        while (position_ < input_.size() && json_is_digit(input_[position_])) ++position_;
    }

    const auto text = input_.substr(start, position_ - start);
    if (floating)
    {
        double value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return fail(error, json_error_code::invalid_number, start);
        handler.number(value);
    }
    else
    {
        std::int64_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return fail(error, json_error_code::invalid_number, start);
        handler.integer(value);
    }
    return true;
}

bool json_parser::consume_literal(std::string_view literal, json_error& error)
{
    if (input_.substr(position_, literal.size()) != literal)
        return fail(error, json_error_code::unexpected_token, position_);
    position_ += literal.size();
    return true;
}

bool json_parser::fail(json_error& error, json_error_code code, std::size_t offset) noexcept
{
    error = {code, offset};
    return false;
}

void json_parser::whitespace() noexcept
{
    while (position_ < input_.size() && json_is_whitespace(input_[position_])) ++position_;
}

} // namespace cw::server
