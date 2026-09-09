#pragma once

#include <cstdint>

namespace cw::server {

enum class implementation_token_kind : std::uint8_t {
    eof,
    identifier,
    integer_literal,
    character_literal,
    string_literal,
    punctuation
};

enum class implementation_punctuation : std::uint8_t {
    semicolon,
    comma,
    equal,
    dot,
    colon,
    left_bracket,
    right_bracket
};

struct implementation_token {
    implementation_token_kind kind = implementation_token_kind::eof;
    implementation_punctuation punctuation =
        implementation_punctuation::semicolon;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

} // namespace cw::server
