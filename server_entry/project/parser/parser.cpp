#include "parser.hpp"

#include "../../diagnostics/diagnostic_descriptor.hpp"
#include "lexer.hpp"

#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace cw::server {
namespace {

// Implements source-language parsing over one immutable token stream.
// The parser resolves language names through source_environment and writes only
// transient source facts into source_context; Builder remains responsible for
// canonical identity, ABI/layout, invariants, and publication of G.
class parser final {
public:
    parser(
        source_view source_value,
        const source_environment& environment_value,
        operation_id operation_value,
        source_context& output,
        std::span<const parser_token> token_stream) noexcept
        : source(source_value),
          environment(environment_value),
          operation(operation_value),
          context(output),
          tokens(token_stream) {
    }

    status parse_unit(bool nested = false) noexcept {
        try {
            while (peek().kind != parser_token_kind::eof &&
                   !(nested &&
                     peek().punctuation == parser_punctuation::right_brace)) {
                status result;

                if (peek().kind == parser_token_kind::keyword_enum) {
                    result = parse_enum();
                }
                else if (peek().kind == parser_token_kind::keyword_struct) {
                    result = parse_struct();
                }
                else if (peek().kind == parser_token_kind::keyword_namespace) {
                    result = parse_namespace();
                }
                else {
                    return fail(
                        diagnostics::parser_invalid_source,
                        peek().offset,
                        peek().length);
                }

                if (!result.ok()) {
                    return result;
                }
            }

            return {};
        }
        catch (...) {
            return {status_code::initialization_failed};
        }
    }

private:
    static std::uint32_t to_u32(std::size_t value) noexcept {
        return static_cast<std::uint32_t>(value);
    }

    [[nodiscard]] std::string_view text(
        const parser_token& token) const noexcept {
        return source.bytes.substr(token.offset, token.length);
    }

    status fail(
        const diagnostic_descriptor& descriptor,
        std::uint32_t offset,
        std::uint32_t length) noexcept {
        try {
            context.diagnostics.emit({
                descriptor.id,
                descriptor.default_severity,
                operation,
                {source.source, offset, length},
                {}
            });

            return {status_code::configuration_failed};
        }
        catch (...) {
            return {status_code::initialization_failed};
        }
    }

    [[nodiscard]] const parser_token& peek() const noexcept {
        return tokens[position];
    }

    bool take(parser_punctuation punctuation) noexcept {
        if (peek().punctuation != punctuation) {
            return false;
        }

        ++position;
        return true;
    }

    bool take(parser_token_kind kind) noexcept {
        if (peek().kind != kind) {
            return false;
        }

        ++position;
        return true;
    }

    status store_qualified(
        std::string_view local,
        source_name_ref& output) noexcept {
        try {
            std::string qualified;

            if (!scope.empty()) {
                qualified = scope;
                qualified += "::";
            }

            qualified += local;
            return context.store_name(qualified, output);
        }
        catch (...) {
            return {status_code::initialization_failed};
        }
    }

    [[nodiscard]] std::string qualified_name(
        std::string_view local) const {
        if (scope.empty()) {
            return std::string{local};
        }

        std::string qualified = scope;
        qualified += "::";
        qualified += local;
        return qualified;
    }

    status register_local_type(std::string_view local) noexcept {
        try {
            local_types.insert(qualified_name(local));
            return {};
        }
        catch (...) {
            return {status_code::initialization_failed};
        }
    }

    bool lookup_type(
        std::string_view name,
        std::uint32_t source_offset,
        std::string& canonical) const noexcept {

        canonical.clear();
        auto current_scope = std::string_view{scope};

        for (;;) {
            try {
                std::string candidate;

                if (!current_scope.empty()) {
                    candidate.assign(current_scope);
                    candidate += "::";
                }

                candidate += name;

                if (local_types.contains(candidate)) {
                    canonical = std::move(candidate);
                    return true;
                }
            }
            catch (...) {
                return false;
            }

            std::string_view imported;

            if (environment.find_type_exact(
                    current_scope,
                    name,
                    source_offset,
                    imported).ok()) {
                try {
                    canonical.assign(imported);
                    return true;
                }
                catch (...) {
                    return false;
                }
            }

            if (current_scope.empty()) {
                break;
            }

            const auto parent = current_scope.rfind("::");
            current_scope =
                parent == std::string_view::npos
                    ? std::string_view{}
                    : current_scope.substr(0, parent);
        }

        return false;
    }

    status parse_underlying(
        std::optional<builtin_type>& output,
        source_text_range& range) noexcept {
        if (peek().kind != parser_token_kind::identifier) {
            return fail(
                diagnostics::parser_invalid_enum_underlying,
                peek().offset,
                peek().length);
        }

        const auto first = peek();
        range = {first.offset, first.length};
        ++position;

        if (text(first) == "char") {
            output = builtin_type::character;
        }
        else if (text(first) == "short") {
            output = builtin_type::short_integer;
        }
        else if (text(first) == "int") {
            output = builtin_type::integer;
        }
        else if (text(first) == "long") {
            if (peek().kind == parser_token_kind::identifier &&
                text(peek()) == "long") {
                range.length =
                    peek().offset + peek().length - range.offset;
                ++position;
                output = builtin_type::long_long_integer;
            }
            else {
                output = builtin_type::long_integer;
            }
        }
        else if (text(first) == "unsigned") {
            if (peek().kind != parser_token_kind::identifier) {
                output = builtin_type::unsigned_integer;
            }
            else if (text(peek()) == "char") {
                range.length =
                    peek().offset + peek().length - range.offset;
                ++position;
                output = builtin_type::unsigned_character;
            }
            else if (text(peek()) == "short") {
                range.length =
                    peek().offset + peek().length - range.offset;
                ++position;
                output = builtin_type::unsigned_short_integer;
            }
            else if (text(peek()) == "int") {
                range.length =
                    peek().offset + peek().length - range.offset;
                ++position;
                output = builtin_type::unsigned_integer;
            }
            else if (text(peek()) == "long") {
                range.length =
                    peek().offset + peek().length - range.offset;
                ++position;

                if (peek().kind == parser_token_kind::identifier &&
                    text(peek()) == "long") {
                    range.length =
                        peek().offset + peek().length - range.offset;
                    ++position;
                    output = builtin_type::unsigned_long_long_integer;
                }
                else {
                    output = builtin_type::unsigned_long_integer;
                }
            }
            else {
                return fail(
                    diagnostics::parser_invalid_enum_underlying,
                    peek().offset,
                    peek().length);
            }
        }
        else {
            return fail(
                diagnostics::parser_invalid_enum_underlying,
                first.offset,
                first.length);
        }

        return {};
    }

    status parse_number(integral_constant& output) noexcept {
        const bool negative = take(parser_punctuation::minus);

        if (peek().kind != parser_token_kind::integer_literal) {
            return fail(
                diagnostics::parser_invalid_enumerator_expression,
                peek().offset,
                peek().length);
        }

        const auto token = peek();
        ++position;

        const auto spelling = text(token);
        std::uint64_t value = 0;

        const auto conversion =
            std::from_chars(
                spelling.data(),
                spelling.data() + spelling.size(),
                value);

        if (conversion.ec != std::errc{} ||
            conversion.ptr != spelling.data() + spelling.size()) {
            return fail(
                diagnostics::parser_invalid_enumerator_expression,
                token.offset,
                token.length);
        }

        if (negative) {
            if (value > (std::uint64_t{1} << 63)) {
                return fail(
                    diagnostics::parser_invalid_enumerator_expression,
                    token.offset,
                    token.length);
            }

            output = {
                builtin_type::long_long_integer,
                std::uint64_t{0} - value
            };
        }
        else {
            output = {
                value <= static_cast<std::uint64_t>(
                             (std::numeric_limits<std::int64_t>::max)())
                    ? builtin_type::long_long_integer
                    : builtin_type::unsigned_long_long_integer,
                value
            };
        }

        return {};
    }

    bool lookup_constant(
        std::string_view name,
        std::uint32_t source_offset,
        integral_constant& output) const noexcept {
        const auto local = local_constants.find(name);

        if (local != local_constants.end()) {
            output = local->second;
            return true;
        }

        auto current_scope = std::string_view{scope};

        // Name lookup walks the current namespace and then each parent scope.
        for (;;) {
            if (environment.find_constant_exact(
                    current_scope,
                    name,
                    source_offset,
                    output).ok()) {
                return true;
            }

            if (current_scope.empty()) {
                break;
            }

            const auto parent = current_scope.rfind("::");
            current_scope =
                parent == std::string_view::npos
                    ? std::string_view{}
                    : current_scope.substr(0, parent);
        }

        return false;
    }

    status parse_primary(integral_constant& output) noexcept {
        if (peek().kind == parser_token_kind::integer_literal ||
            peek().punctuation == parser_punctuation::minus) {
            return parse_number(output);
        }

        if (peek().kind == parser_token_kind::identifier) {
            const auto token = peek();
            ++position;

            if (lookup_constant(
                    text(token),
                    token.offset,
                    output)) {
                return {};
            }

            return fail(
                diagnostics::parser_invalid_enumerator_expression,
                token.offset,
                token.length);
        }

        return fail(
            diagnostics::parser_invalid_enumerator_expression,
            peek().offset,
            peek().length);
    }

    status parse_expression(integral_constant& output) noexcept {
        auto result = parse_primary(output);

        if (!result.ok()) {
            return result;
        }

        while (take(parser_punctuation::plus)) {
            integral_constant right;
            result = parse_primary(right);

            if (!result.ok()) {
                return result;
            }

            const bool unsigned_result =
                output.type == builtin_type::unsigned_long_long_integer ||
                right.type == builtin_type::unsigned_long_long_integer;

            if (unsigned_result) {
                if ((std::numeric_limits<std::uint64_t>::max)() - output.bits <
                    right.bits) {
                    return fail(
                        diagnostics::parser_invalid_enumerator_expression,
                        peek().offset,
                        peek().length);
                }

                output = {
                    builtin_type::unsigned_long_long_integer,
                    output.bits + right.bits
                };
            }
            else {
                const auto left_value =
                    static_cast<std::int64_t>(output.bits);
                const auto right_value =
                    static_cast<std::int64_t>(right.bits);

                const bool overflow =
                    (right_value > 0 &&
                     left_value >
                         (std::numeric_limits<std::int64_t>::max)() -
                             right_value) ||
                    (right_value < 0 &&
                     left_value <
                         (std::numeric_limits<std::int64_t>::min)() -
                             right_value);

                if (overflow) {
                    return fail(
                        diagnostics::parser_invalid_enumerator_expression,
                        peek().offset,
                        peek().length);
                }

                output = {
                    builtin_type::long_long_integer,
                    static_cast<std::uint64_t>(
                        left_value + right_value)
                };
            }
        }

        return {};
    }

    status increment_constant(
        integral_constant& value,
        const parser_token& location) noexcept {
        if (value.type == builtin_type::unsigned_long_long_integer) {
            if (value.bits ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                return fail(
                    diagnostics::parser_invalid_enumerator_expression,
                    location.offset,
                    location.length);
            }
        }
        else if (value.bits ==
                 static_cast<std::uint64_t>(
                     (std::numeric_limits<std::int64_t>::max)())) {
            return fail(
                diagnostics::parser_invalid_enumerator_expression,
                location.offset,
                location.length);
        }

        ++value.bits;
        return {};
    }

    status parse_enum() noexcept {
        const auto declaration = peek();
        ++position;

        const bool scoped_enum =
            take(parser_token_kind::keyword_class);

        parser_token name_token{};

        if (peek().kind == parser_token_kind::identifier) {
            name_token = peek();
            ++position;
        }

        const bool anonymous = name_token.length == 0;

        std::optional<builtin_type> underlying;
        source_text_range underlying_range;

        if (take(parser_punctuation::colon)) {
            auto result =
                parse_underlying(
                    underlying,
                    underlying_range);

            if (!result.ok()) {
                return result;
            }
        }

        enum_declaration_source_fact fact;
        fact.anonymous = anonymous;
        fact.scoped = scoped_enum;
        fact.explicit_underlying = underlying;
        fact.declaration_range = {declaration.offset, 0};
        fact.name_range = {name_token.offset, name_token.length};
        fact.underlying_range = underlying_range;

        if (!scope.empty()) {
            auto result =
                context.store_name(
                    scope,
                    fact.scope_name);

            if (!result.ok()) {
                return result;
            }
        }

        if (!anonymous) {
            auto result =
                store_qualified(
                    text(name_token),
                    fact.canonical_name);

            if (!result.ok()) {
                return result;
            }

            result = register_local_type(text(name_token));

            if (!result.ok()) {
                return result;
            }
        }

        fact.enumerator_offset =
            to_u32(context.enum_values.size());

        if (take(parser_punctuation::semicolon)) {
            if (anonymous) {
                return fail(
                    diagnostics::parser_anonymous_opaque_enum,
                    declaration.offset,
                    declaration.length);
            }

            if (!scoped_enum && !underlying) {
                return fail(
                    diagnostics::parser_invalid_enum_forward_declaration,
                    declaration.offset,
                    declaration.length);
            }

            fact.definition_state = enum_definition_state::opaque;
            fact.declaration_range.length =
                peek().offset - fact.declaration_range.offset;

            context.enums.push_back(fact);
            return {};
        }

        if (!take(parser_punctuation::left_brace)) {
            return fail(
                diagnostics::parser_invalid_source,
                peek().offset,
                peek().length);
        }

        // Enumerator names are local to the enum currently being parsed.
        local_constants.clear();

        integral_constant next{
            builtin_type::long_long_integer,
            0
        };

        while (!take(parser_punctuation::right_brace)) {
            if (peek().kind != parser_token_kind::identifier) {
                return fail(
                    diagnostics::parser_expected_enumerator_identifier,
                    peek().offset,
                    peek().length);
            }

            const auto name = peek();
            ++position;

            if (local_constants.contains(text(name))) {
                return fail(
                    diagnostics::parser_duplicate_enumerator,
                    name.offset,
                    name.length);
            }

            source_name_ref stored_name;
            auto result =
                context.store_name(
                    text(name),
                    stored_name);

            if (!result.ok()) {
                return result;
            }

            integral_constant value = next;
            source_text_range expression_range{
                name.offset,
                name.length
            };

            if (take(parser_punctuation::equal)) {
                const auto expression_start = peek().offset;

                result = parse_expression(value);

                if (!result.ok()) {
                    return result;
                }

                expression_range = {
                    expression_start,
                    peek().offset - expression_start
                };
            }

            local_constants.emplace(text(name), value);

            context.enum_values.push_back({
                stored_name,
                value,
                {name.offset, name.length},
                expression_range
            });

            ++fact.enumerator_count;

            if (!take(parser_punctuation::comma)) {
                if (peek().punctuation !=
                    parser_punctuation::right_brace) {
                    return fail(
                        diagnostics::parser_invalid_source,
                        peek().offset,
                        peek().length);
                }

                continue;
            }

            if (peek().punctuation ==
                parser_punctuation::right_brace) {
                continue;
            }

            next = value;
            result = increment_constant(next, name);

            if (!result.ok()) {
                return result;
            }
        }

        if (!take(parser_punctuation::semicolon)) {
            return fail(
                diagnostics::parser_expected_semicolon,
                peek().offset,
                peek().length);
        }

        fact.declaration_range.length =
            peek().offset - fact.declaration_range.offset;

        context.enums.push_back(fact);
        return {};
    }

    status parse_namespace() noexcept {
        const auto declaration = peek();
        ++position;

        if (peek().kind != parser_token_kind::identifier) {
            return fail(
                diagnostics::parser_invalid_source,
                declaration.offset,
                declaration.length);
        }

        const auto previous_scope_size = scope.size();

        if (!scope.empty()) {
            scope += "::";
        }

        scope += text(peek());
        ++position;

        if (!take(parser_punctuation::left_brace)) {
            return fail(
                diagnostics::parser_invalid_source,
                peek().offset,
                peek().length);
        }

        auto result = parse_unit(true);

        if (!result.ok()) {
            return result;
        }

        if (!take(parser_punctuation::right_brace)) {
            return fail(
                diagnostics::parser_invalid_source,
                peek().offset,
                peek().length);
        }

        scope.resize(previous_scope_size);
        return {};
    }

    status parse_struct() noexcept {
        const auto declaration = peek();
        ++position;

        if (peek().kind != parser_token_kind::identifier) {
            return fail(
                diagnostics::parser_invalid_source,
                peek().offset,
                peek().length);
        }

        const auto name = peek();
        ++position;

        aggregate_declaration_source_fact fact;
        fact.name_range = {name.offset, name.length};
        fact.declaration_range = {declaration.offset, 0};

        auto result =
            store_qualified(
                text(name),
                fact.canonical_name);

        if (!result.ok()) {
            return result;
        }

        result = register_local_type(text(name));

        if (!result.ok()) {
            return result;
        }

        if (!scope.empty()) {
            result =
                context.store_name(
                    scope,
                    fact.scope_name);

            if (!result.ok()) {
                return result;
            }
        }

        if (take(parser_punctuation::semicolon)) {
            fact.definition_state =
                aggregate_definition_state::declared;
        }
        else {
            if (!take(parser_punctuation::left_brace)) {
                return fail(
                    diagnostics::parser_invalid_source,
                    peek().offset,
                    peek().length);
            }

            fact.member_offset =
                static_cast<std::uint32_t>(
                    context.aggregate_members.size());

            while (!take(parser_punctuation::right_brace)) {
                const auto member_start = peek();

                if (member_start.kind !=
                    parser_token_kind::identifier) {
                    return fail(
                        diagnostics::parser_invalid_source,
                        member_start.offset,
                        member_start.length);
                }

                ++position;

                member_declaration_source_fact member;
                member.type_range = {
                    member_start.offset,
                    member_start.length
                };

                member.modifier_offset =
                    static_cast<std::uint32_t>(
                        context.type_modifiers.size());

                if (text(member_start) == "int") {
                    member.builtin = builtin_type::integer;
                }
                else {
                    std::string canonical;

                    if (!lookup_type(
                            text(member_start),
                            member_start.offset,
                            canonical)) {
                        return fail(
                            diagnostics::parser_invalid_source,
                            member_start.offset,
                            member_start.length);
                    }

                    result =
                        context.store_name(
                            canonical,
                            member.type_name);

                    if (!result.ok()) {
                        return result;
                    }
                }

                while (peek().punctuation ==
                       parser_punctuation::asterisk) {
                    const auto modifier = peek();
                    ++position;

                    context.type_modifiers.push_back({
                        source_type_modifier_kind::pointer,
                        0,
                        {modifier.offset, modifier.length}
                    });

                    ++member.modifier_count;
                    member.type_range.length =
                        modifier.offset + modifier.length -
                        member.type_range.offset;
                }

                if (peek().punctuation ==
                    parser_punctuation::ampersand) {
                    const auto first = peek();
                    ++position;

                    const bool rvalue =
                        peek().punctuation ==
                        parser_punctuation::ampersand;

                    source_text_range range{
                        first.offset,
                        first.length
                    };

                    if (rvalue) {
                        range.length =
                            peek().offset + peek().length -
                            range.offset;
                        ++position;
                    }

                    context.type_modifiers.push_back({
                        rvalue
                            ? source_type_modifier_kind::rvalue_reference
                            : source_type_modifier_kind::lvalue_reference,
                        0,
                        range
                    });

                    ++member.modifier_count;
                    member.type_range.length =
                        range.offset + range.length -
                        member.type_range.offset;
                }

                if (peek().kind != parser_token_kind::identifier) {
                    return fail(
                        diagnostics::parser_invalid_source,
                        peek().offset,
                        peek().length);
                }

                const auto member_name = peek();
                ++position;

                result =
                    context.store_name(
                        text(member_name),
                        member.name);

                if (!result.ok()) {
                    return result;
                }

                member.name_range = {
                    member_name.offset,
                    member_name.length
                };

                while (take(parser_punctuation::left_bracket)) {
                    const auto open =
                        tokens[position - 1];

                    if (peek().kind !=
                        parser_token_kind::integer_literal) {
                        return fail(
                            diagnostics::parser_invalid_source,
                            peek().offset,
                            peek().length);
                    }

                    const auto extent_token = peek();
                    ++position;

                    std::uint64_t extent = 0;
                    const auto spelling = text(extent_token);
                    const auto conversion =
                        std::from_chars(
                            spelling.data(),
                            spelling.data() + spelling.size(),
                            extent);

                    if (conversion.ec != std::errc{} ||
                        conversion.ptr !=
                            spelling.data() + spelling.size() ||
                        extent == 0 ||
                        !take(parser_punctuation::right_bracket)) {
                        return fail(
                            diagnostics::parser_invalid_source,
                            extent_token.offset,
                            extent_token.length);
                    }

                    const auto close =
                        tokens[position - 1];

                    const auto existing =
                        context.modifiers(member);

                    if (!existing.empty()) {
                        const auto last_kind =
                            existing.back().kind;

                        if (last_kind ==
                                source_type_modifier_kind::lvalue_reference ||
                            last_kind ==
                                source_type_modifier_kind::rvalue_reference) {
                            return fail(
                                diagnostics::parser_invalid_source,
                                open.offset,
                                close.offset + close.length -
                                    open.offset);
                        }
                    }

                    context.type_modifiers.push_back({
                        source_type_modifier_kind::array,
                        extent,
                        {
                            open.offset,
                            close.offset + close.length -
                                open.offset
                        }
                    });

                    ++member.modifier_count;
                }

                if (!take(parser_punctuation::semicolon)) {
                    return fail(
                        diagnostics::parser_expected_semicolon,
                        peek().offset,
                        peek().length);
                }

                member.declaration_range = {
                    member_start.offset,
                    peek().offset - member_start.offset
                };

                context.aggregate_members.push_back(member);
                ++fact.member_count;
            }

            if (!take(parser_punctuation::semicolon)) {
                return fail(
                    diagnostics::parser_expected_semicolon,
                    peek().offset,
                    peek().length);
            }

            fact.definition_state =
                aggregate_definition_state::defined;
        }

        fact.declaration_range.length =
            peek().offset - declaration.offset;

        context.aggregates.push_back(fact);
        return {};
    }

    source_view source;
    const source_environment& environment;
    operation_id operation;
    source_context& context;
    std::span<const parser_token> tokens;
    std::size_t position = 0;
    std::string scope;
    std::unordered_map<std::string_view, integral_constant> local_constants;
    std::unordered_set<std::string> local_types;
};

void emit_initialization_failure_if_needed(
    source_view source,
    operation_id operation,
    source_context& context,
    status result) noexcept {
    if (result.code != status_code::initialization_failed ||
        !context.diagnostics.empty()) {
        return;
    }

    try {
        context.diagnostics.emit({
            diagnostics::parser_initialization_failed.id,
            diagnostics::parser_initialization_failed.default_severity,
            operation,
            {source.source, 0, 0},
            {}
        });
    }
    catch (...) {
        // status is the unavoidable fallback when diagnostic storage itself failed.
    }
}

} // namespace

status parse_source_tokens(
    source_view source,
    std::span<const parser_token> tokens,
    const source_environment& environment,
    operation_id operation,
    source_context& context) noexcept {
    context.reset();

    status result;

    if (tokens.empty() ||
        tokens.back().kind != parser_token_kind::eof) {
        result = {status_code::configuration_failed};
    }
    else {
        parser instance{
            source,
            environment,
            operation,
            context,
            tokens
        };

        result = instance.parse_unit();
    }

    emit_initialization_failure_if_needed(
        source,
        operation,
        context,
        result);

    return result;
}

status parse_source(
    source_view source,
    const source_environment& environment,
    operation_id operation,
    source_context& context) noexcept {
    context.reset();

    auto result =
        lex_source(
            source,
            operation,
            context.diagnostics,
            context.tokens);

    if (!result.ok()) {
        return result;
    }

    parser instance{
        source,
        environment,
        operation,
        context,
        context.tokens
    };

    result = instance.parse_unit();

    emit_initialization_failure_if_needed(
        source,
        operation,
        context,
        result);

    return result;
}

status native_parser_backend::parse(
    source_view source,
    std::span<const parser_token> tokens,
    const source_environment& environment,
    const language_configuration& language,
    operation_id operation,
    source_context& context) const noexcept {
    (void)language;

    return parse_source_tokens(
        source,
        tokens,
        environment,
        operation,
        context);
}

const parser_backend& default_parser_backend() noexcept {
    static const native_parser_backend backend;
    return backend;
}

} // namespace cw::server
