#pragma once

#include "json_buffer.hpp"
#include "json_error.hpp"

#include <cstdint>
#include <string_view>

namespace cw::server {

// Receives structural and scalar events emitted by json_parser.
// String and key views are transient parser-owned data and must be copied by a
// handler that needs to retain them after the callback returns.
class json_event_handler {
public:
    virtual ~json_event_handler() = default;

    virtual void location(std::size_t) noexcept {}
    virtual void object_begin() noexcept {}
    virtual void object_end() noexcept {}
    virtual void array_begin() noexcept {}
    virtual void array_end() noexcept {}
    virtual void key(std::string_view) noexcept {}
    virtual void string(std::string_view) noexcept {}
    virtual void integer(std::int64_t) noexcept {}
    virtual void number(double) noexcept {}
    virtual void boolean(bool) noexcept {}
    virtual void null() noexcept {}
};

// Parses JSON directly into json_event_handler callbacks without building a DOM.
// json_parser does not own the input text; the referenced storage must remain
// valid for the parser lifetime and for the duration of parse().
class json_parser {
public:
    explicit json_parser(std::string_view input) noexcept : input(input) {}

    [[nodiscard]] bool parse(json_event_handler& handler, json_error& error) noexcept;

private:
    bool parse_value(json_event_handler& handler, json_error& error);
    bool parse_object(json_event_handler& handler, json_error& error);
    bool parse_array(json_event_handler& handler, json_error& error);
    bool parse_string(json_buffer& output, json_error& error);
    bool parse_number(json_event_handler& handler, json_error& error);
    bool consume_literal(std::string_view literal, json_error& error);
    bool fail(json_error& error, json_error_code code, std::size_t offset) noexcept;
    void whitespace() noexcept;

    std::string_view input;
    std::size_t position = 0;
    json_buffer string_buffer;
};

} // namespace cw::server
