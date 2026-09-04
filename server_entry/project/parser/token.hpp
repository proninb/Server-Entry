#pragma once

#include <cstdint>

namespace cw::server {

// Classifies one lexical token produced from a Source snapshot.
// Token kinds describe only lexical categories; source-language meaning beyond
// these reserved keywords is interpreted by Parser.
enum class parser_token_kind : std::uint8_t {
    eof,
    identifier,
    integer_literal,
    keyword_namespace,
    keyword_enum,
    keyword_class,
    keyword_struct,
    string_literal,
    punctuation
};

// Identifies punctuation recognized by the current language lexer.
// The enumeration is intentionally limited to punctuation required by the
// supported source-language grammar.
enum class parser_punctuation : std::uint8_t {
    none,
    left_brace,
    right_brace,
    semicolon,
    colon,
    comma,
    equal,
    plus,
    minus,
    hash,
    ampersand,
    asterisk,
    left_bracket,
    right_bracket
};

// Describes one compact source-local lexical token.
// offset and length address bytes in the Source snapshot; the token owns no text.
// flags bit 0 marks the first non-trivia token on a source line.
struct parser_token {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    parser_token_kind kind = parser_token_kind::eof;
    parser_punctuation punctuation = parser_punctuation::none;
    std::uint8_t flags = 0;
};

// Tokens are kept compact because every parsed Source materializes a token array.
static_assert(sizeof(parser_token) <= 12);

} // namespace cw::server
