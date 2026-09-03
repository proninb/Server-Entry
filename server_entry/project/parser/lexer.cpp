#include "lexer.hpp"

#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <limits>

namespace cw::server
{
namespace
{
bool alpha(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           value == '_';
}

status fail(source_view source, operation_id operation,
            diagnostic_buffer& diagnostics, std::size_t offset,
            std::size_t length) noexcept
{
    try
    {
        diagnostics.emit({diagnostics::parser_invalid_source.id,
            diagnostics::parser_invalid_source.default_severity, operation,
            {source.source, static_cast<std::uint32_t>(offset),
             static_cast<std::uint32_t>(length)}, {}});
    }
    catch (...) { return {status_code::initialization_failed}; }
    return {status_code::configuration_failed};
}
}

status lex_source(source_view source, operation_id operation,
                  diagnostic_buffer& diagnostics,
                  std::vector<parser_token>& output,
                  std::vector<directive_span>* directives) noexcept
{
    output.clear();
    if (directives) directives->clear();
    if (source.bytes.size() > (std::numeric_limits<std::uint32_t>::max)())
        return fail(source, operation, diagnostics, 0, 0);
    try
    {
        bool line_start = true;
        std::uint32_t directive_begin = 0;
        bool directive_open = false;
        for (std::size_t index = 0; index < source.bytes.size();)
        {
            const auto character = source.bytes[index];
            if (character == ' ' || character == '\t' || character == '\r')
            { ++index; continue; }
            if (character == '\n')
            {
                if (directive_open && directives)
                    directives->push_back({directive_begin,
                        static_cast<std::uint32_t>(output.size())});
                directive_open = false;
                ++index;
                line_start = true;
                continue;
            }
            if (character == '/' && index + 1 < source.bytes.size() &&
                source.bytes[index + 1] == '/')
            {
                index += 2;
                while (index < source.bytes.size() && source.bytes[index] != '\n')
                    ++index;
                continue;
            }
            if (character == '/' && index + 1 < source.bytes.size() &&
                source.bytes[index + 1] == '*')
            {
                const auto start = index;
                index += 2;
                while (index + 1 < source.bytes.size() &&
                       !(source.bytes[index] == '*' && source.bytes[index + 1] == '/'))
                {
                    if (source.bytes[index] == '\n') line_start = true;
                    ++index;
                }
                if (index + 1 >= source.bytes.size())
                    return fail(source, operation, diagnostics, start,
                                source.bytes.size() - start);
                index += 2;
                continue;
            }
            const auto start = index;
            parser_token token;
            token.offset = static_cast<std::uint32_t>(start);
            token.flags = line_start ? 1 : 0;
            line_start = false;
            if (alpha(character))
            {
                ++index;
                while (index < source.bytes.size() &&
                       (alpha(source.bytes[index]) ||
                        (source.bytes[index] >= '0' && source.bytes[index] <= '9')))
                    ++index;
                const auto text = source.bytes.substr(start, index - start);
                token.kind = text == "namespace" ? parser_token_kind::keyword_namespace :
                    text == "enum" ? parser_token_kind::keyword_enum :
                    text == "class" ? parser_token_kind::keyword_class :
                    text == "struct" ? parser_token_kind::keyword_struct :
                    parser_token_kind::identifier;
            }
            else if (character >= '0' && character <= '9')
            {
                ++index;
                while (index < source.bytes.size() && source.bytes[index] >= '0' &&
                       source.bytes[index] <= '9') ++index;
                token.kind = parser_token_kind::integer_literal;
            }
            else if (character == '"')
            {
                ++index;
                while (index < source.bytes.size() && source.bytes[index] != '"' &&
                       source.bytes[index] != '\n') ++index;
                if (index == source.bytes.size() || source.bytes[index] != '"')
                    return fail(source, operation, diagnostics, start, index - start);
                ++index;
                token.kind = parser_token_kind::string_literal;
            }
            else
            {
                token.kind = parser_token_kind::punctuation;
                switch (character)
                {
                case '{': token.punctuation = parser_punctuation::left_brace; break;
                case '}': token.punctuation = parser_punctuation::right_brace; break;
                case ';': token.punctuation = parser_punctuation::semicolon; break;
                case ':': token.punctuation = parser_punctuation::colon; break;
                case ',': token.punctuation = parser_punctuation::comma; break;
                case '=': token.punctuation = parser_punctuation::equal; break;
                case '+': token.punctuation = parser_punctuation::plus; break;
                case '-': token.punctuation = parser_punctuation::minus; break;
                case '#': token.punctuation = parser_punctuation::hash; break;
                case '&': token.punctuation = parser_punctuation::ampersand; break;
                default: return fail(source, operation, diagnostics, index, 1);
                }
                ++index;
            }
            token.length = static_cast<std::uint32_t>(index - start);
            if (token.punctuation == parser_punctuation::hash &&
                (token.flags & 1) != 0 && directives)
            {
                directive_begin = static_cast<std::uint32_t>(output.size());
                directive_open = true;
            }
            output.push_back(token);
        }
        if (directive_open && directives)
            directives->push_back({directive_begin,
                static_cast<std::uint32_t>(output.size())});
        output.push_back({static_cast<std::uint32_t>(source.bytes.size()), 0,
                          parser_token_kind::eof, parser_punctuation::none, 0});
        return {};
    }
    catch (...)
    {
        output.clear();
        if (directives) directives->clear();
        return {status_code::initialization_failed};
    }
}
}
