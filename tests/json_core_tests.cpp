#include "../server_entry/json/json_parser.hpp"
#include "../server_entry/json/json_writer.hpp"

#include <cmath>
#include <string>
#include <type_traits>
#include <utility>

namespace
{

class capture_handler final : public cw::server::json_event_handler
{
public:
    void object_begin() noexcept override { ++objects; }
    void array_begin() noexcept override { ++arrays; }
    void string(std::string_view value) noexcept override
    {
        try { last_string.assign(value); } catch (...) { failed = true; }
    }
    void integer(std::int64_t value) noexcept override { last_integer = value; ++integers; }
    void number(double value) noexcept override { last_number = value; ++numbers; }
    void boolean(bool value) noexcept override { last_boolean = value; ++booleans; }
    void null() noexcept override { ++nulls; }

    int objects = 0;
    int arrays = 0;
    int integers = 0;
    int numbers = 0;
    int booleans = 0;
    int nulls = 0;
    bool failed = false;
    bool last_boolean = false;
    std::int64_t last_integer = 0;
    double last_number = 0;
    std::string last_string;
};

static_assert(noexcept(std::declval<cw::server::json_parser&>().parse(
    std::declval<cw::server::json_event_handler&>(), std::declval<cw::server::json_error&>())));

bool parses(std::string_view text, capture_handler& handler)
{
    cw::server::json_error error;
    cw::server::json_parser parser{text};
    return parser.parse(handler, error) && error.ok() && !handler.failed;
}

bool test_json_values()
{
    capture_handler handler;
    if (!parses("{}", handler) || handler.objects != 1) return false;
    handler = {};
    if (!parses("[]", handler) || handler.arrays != 1) return false;
    handler = {};
    if (!parses("\"escaped\\nstring\"", handler) || handler.last_string != "escaped\nstring") return false;
    handler = {};
    if (!parses("\"\\u0041\\uD83D\\uDE00\"", handler) || handler.last_string != "A\xF0\x9F\x98\x80") return false;
    handler = {};
    if (!parses("-42", handler) || handler.last_integer != -42) return false;
    handler = {};
    if (!parses("1.25", handler) || handler.numbers != 1 || std::abs(handler.last_number - 1.25) > 0.0001) return false;
    handler = {};
    if (!parses("2e3", handler) || handler.last_number != 2000.0) return false;
    handler = {};
    if (!parses("true", handler) || !handler.last_boolean) return false;
    handler = {};
    if (!parses("null", handler) || handler.nulls != 1) return false;
    handler = {};
    return parses("{\"a\":[1,{\"b\":false}]}", handler) && handler.objects == 2 &&
           handler.arrays == 1 && handler.integers == 1 && handler.booleans == 1;
}

bool fails(std::string_view text, cw::server::json_error_code expected)
{
    capture_handler handler;
    cw::server::json_error error;
    cw::server::json_parser parser{text};
    return !parser.parse(handler, error) && error.code == expected;
}

bool test_json_errors()
{
    using cw::server::json_error_code;
    return fails("@", json_error_code::unexpected_token) &&
           fails("{", json_error_code::unexpected_end) &&
           fails("\"\\q\"", json_error_code::invalid_escape) &&
           fails("\"\\uD800\"", json_error_code::invalid_unicode) &&
           fails("01", json_error_code::invalid_number) &&
           fails("true false", json_error_code::trailing_characters);
}

bool test_writer()
{
    using namespace cw::server;
    json_buffer buffer;
    json_writer writer{buffer};
    if (!writer.begin_object() || !writer.key("text") || !writer.string("a\n\"b") ||
        !writer.key("values") || !writer.begin_array() || !writer.integer(-2) ||
        !writer.number(1.5) || !writer.boolean(true) || !writer.null() ||
        !writer.end_array() || !writer.end_object() || !writer.complete()) return false;
    if (buffer.view() != "{\"text\":\"a\\n\\\"b\",\"values\":[-2,1.5,true,null]}") return false;

    capture_handler handler;
    return parses(buffer.view(), handler) && handler.objects == 1 && handler.arrays == 1;
}

} // namespace

int main()
{
    return test_json_values() && test_json_errors() && test_writer() ? 0 : 1;
}
