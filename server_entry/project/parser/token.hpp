#pragma once

#include <cstdint>

namespace cw::server
{
enum class parser_token_kind : std::uint8_t
{
    eof, identifier, integer_literal,
    keyword_namespace, keyword_enum, keyword_class, keyword_struct,
    string_literal, punctuation
};
enum class parser_punctuation : std::uint8_t
{
    none, left_brace, right_brace, semicolon, colon, comma, equal, plus, minus,
    hash, ampersand
};
struct parser_token
{
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    parser_token_kind kind = parser_token_kind::eof;
    parser_punctuation punctuation = parser_punctuation::none;
    std::uint8_t flags = 0;
};
static_assert(sizeof(parser_token) <= 12);
} // namespace cw::server
