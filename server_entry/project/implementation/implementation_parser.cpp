#include "implementation_parser.hpp"

#include "implementation_lexer.hpp"
#include "../graph/graph.hpp"
#include "../string/string_registry.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <charconv>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace cw::server {
namespace {

struct implementation_rhs {
    bool path = false;
    std::uint32_t coordinate = 0;
};

class implementation_parser final {
public:
    implementation_parser(
        source_view source_value,
        const graph_type_view& types_value,
        const string_registry& strings_value,
        operation_id operation_value,
        implementation_context& context_value) noexcept
        : source(source_value),
          types(types_value),
          strings(strings_value),
          operation(operation_value),
          context(context_value) {}

    [[nodiscard]] status parse() {
        while (peek().kind !=
               implementation_token_kind::eof) {
            const auto result =
                parse_statement();

            if (!result.ok()) {
                return result;
            }
        }

        return {};
    }

private:
    [[nodiscard]] const implementation_token& peek(
        std::size_t lookahead = 0) const noexcept {

        const auto index =
            position + lookahead;

        return index < context.tokens.size()
            ? context.tokens[index]
            : context.tokens.back();
    }

    [[nodiscard]] std::string_view text(
        const implementation_token& token) const noexcept {

        const auto end =
            std::uint64_t{token.offset} +
            token.length;

        return end <= source.bytes.size()
            ? source.bytes.substr(
                  token.offset,
                  token.length)
            : std::string_view{};
    }

    [[nodiscard]] bool punctuation(
        const implementation_token& token,
        implementation_punctuation value) const noexcept {

        return
            token.kind ==
                implementation_token_kind::punctuation &&
            token.punctuation == value;
    }

    [[nodiscard]] bool take(
        implementation_punctuation value) noexcept {

        if (!punctuation(peek(), value)) {
            return false;
        }

        ++position;
        return true;
    }

    [[nodiscard]] status fail(
        const diagnostic_descriptor& descriptor,
        const implementation_token& token) noexcept {

        try {
            context.diagnostics.emit({
                descriptor.id,
                descriptor.default_severity,
                operation,
                {
                    source.source,
                    token.offset,
                    token.length
                },
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

    [[nodiscard]] bool declaration_ahead() const noexcept {
        if (peek().kind !=
            implementation_token_kind::identifier) {
            return false;
        }

        auto cursor = position + 1;

        while (cursor + 2 < context.tokens.size() &&
               punctuation(
                   context.tokens[cursor],
                   implementation_punctuation::colon) &&
               punctuation(
                   context.tokens[cursor + 1],
                   implementation_punctuation::colon) &&
               context.tokens[cursor + 2].kind ==
                   implementation_token_kind::identifier) {
            cursor += 3;
        }

        return
            cursor < context.tokens.size() &&
            context.tokens[cursor].kind ==
                implementation_token_kind::identifier;
    }

    [[nodiscard]] bool builtin_type_from_name(
        std::string_view value,
        builtin_type& output) const noexcept {

        if (value == "bool") {
            output = builtin_type::boolean;
            return true;
        }

        if (value == "char") {
            output = builtin_type::character;
            return true;
        }

        if (value == "int") {
            output = builtin_type::integer;
            return true;
        }

        if (value == "float") {
            output = builtin_type::floating;
            return true;
        }

        if (value == "double") {
            output = builtin_type::double_floating;
            return true;
        }

        return false;
    }

    [[nodiscard]] status parse_type(
        TypeRef& output,
        source_text_range& range) {

        output = {};
        range = {};

        if (peek().kind !=
            implementation_token_kind::identifier) {
            return fail(
                diagnostics::implementation_unknown_type,
                peek());
        }

        const auto first = peek();
        auto last = first;
        std::string qualified{text(first)};
        ++position;

        while (take(
            implementation_punctuation::colon)) {
            if (!take(
                    implementation_punctuation::colon) ||
                peek().kind !=
                    implementation_token_kind::identifier) {
                return fail(
                    diagnostics::implementation_invalid_source,
                    peek());
            }

            qualified += "::";
            qualified += text(peek());
            last = peek();
            ++position;
        }

        range = {
            first.offset,
            last.offset + last.length -
                first.offset
        };

        builtin_type builtin;

        if (qualified.find("::") ==
                std::string::npos &&
            builtin_type_from_name(
                qualified,
                builtin)) {
            output =
                types.type_ref(builtin);

            return output
                ? status{}
                : fail(
                    diagnostics::implementation_unknown_type,
                    first);
        }

        const auto name =
            strings.find_for_construction(
                qualified);

        const auto* entity =
            name
                ? types.find(name)
                : nullptr;

        if (!entity) {
            return fail(
                diagnostics::implementation_unknown_type,
                first);
        }

        output =
            types.type_ref(
                entity->type);

        return output
            ? status{}
            : fail(
                diagnostics::implementation_unknown_type,
                first);
    }

    [[nodiscard]] status parse_integer(
        std::uint64_t& value,
        bool require_nonzero) {

        value = 0;

        if (peek().kind !=
            implementation_token_kind::integer_literal) {
            return fail(
                diagnostics::implementation_invalid_source,
                peek());
        }

        const auto token = peek();
        const auto spelling = text(token);

        const auto conversion =
            std::from_chars(
                spelling.data(),
                spelling.data() +
                    spelling.size(),
                value);

        if (conversion.ec != std::errc{} ||
            conversion.ptr !=
                spelling.data() +
                    spelling.size() ||
            (require_nonzero && value == 0)) {
            return fail(
                diagnostics::implementation_invalid_source,
                token);
        }

        ++position;
        return {};
    }

    [[nodiscard]] TypeRef value_type(
        TypeRef type) const noexcept {

        for (std::size_t depth = 0;
             depth != 64 && type;
             ++depth) {
            const auto* derived =
                types.derived(type);

            if (!derived ||
                (derived->kind !=
                     derived_type_kind::lvalue_reference &&
                 derived->kind !=
                     derived_type_kind::rvalue_reference)) {
                return type;
            }

            type = derived->child;
        }

        return {};
    }

    [[nodiscard]] bool copyable_value_type(
        TypeRef type) const noexcept {

        type = value_type(type);

        if (!type) {
            return false;
        }

        builtin_type builtin;

        if (types.builtin(type, builtin)) {
            return builtin != builtin_type::void_type;
        }

        if (const auto* derived =
                types.derived(type)) {
            return derived->kind ==
                derived_type_kind::pointer;
        }

        type_handle named;

        if (!types.named(type, named)) {
            return false;
        }

        const auto* entry =
            types.find(named);

        return entry &&
            entry->kind == user_type_kind::enumeration;
    }

    [[nodiscard]] status append_path(
        implementation_path_fact fact,
        std::uint32_t& path) {

        path = 0;

        if (!fact.object_index ||
            !fact.type ||
            context.paths.size() >=
                (std::numeric_limits<std::uint32_t>::max)()) {
            return {
                status_code::configuration_failed
            };
        }

        context.paths.push_back(fact);

        path =
            static_cast<std::uint32_t>(
                context.paths.size());

        return {};
    }

    [[nodiscard]] status make_root_path(
        std::uint32_t object_index,
        std::uint32_t& path) {

        path = 0;

        if (!object_index ||
            object_index > context.objects.size()) {
            return {
                status_code::configuration_failed
            };
        }

        const auto& object =
            context.objects[
                object_index - 1];

        return append_path({
            object_index,
            static_cast<std::uint32_t>(
                context.path_steps.size()),
            0,
            object.dimension_count,
            object.type,
            object.name_range
        }, path);
    }

    [[nodiscard]] status parse_path(
        std::uint32_t& path) {

        path = 0;

        if (peek().kind !=
            implementation_token_kind::identifier) {
            return fail(
                diagnostics::implementation_unknown_object,
                peek());
        }

        const auto root_token = peek();
        const auto root_name =
            text(root_token);
        ++position;

        std::uint32_t object_index = 0;

        auto result =
            context.find_object(
                root_name,
                object_index);

        if (!result.ok()) {
            if (result.code ==
                status_code::initialization_failed) {
                return result;
            }

            return fail(
                diagnostics::implementation_unknown_object,
                root_token);
        }

        if (!object_index ||
            object_index > context.objects.size()) {
            return {
                status_code::configuration_failed
            };
        }

        const auto& object =
            context.objects[
                object_index - 1];

        auto current_type =
            object.type;

        auto remaining_dimensions =
            object.dimension_count;

        std::uint32_t consumed_dimensions = 0;

        const auto step_offset =
            static_cast<std::uint32_t>(
                context.path_steps.size());

        auto end =
            root_token.offset +
            root_token.length;

        while (true) {
            if (take(
                    implementation_punctuation::left_bracket)) {
                const auto index_token = peek();
                std::uint64_t index = 0;

                result =
                    parse_integer(
                        index,
                        false);

                if (!result.ok()) {
                    return result;
                }

                if (!take(
                        implementation_punctuation::right_bracket)) {
                    return fail(
                        diagnostics::implementation_invalid_source,
                        peek());
                }

                if (remaining_dimensions != 0) {
                    const auto dimension =
                        static_cast<std::size_t>(
                            object.dimension_offset) +
                        consumed_dimensions;

                    if (dimension >=
                            context.object_dimensions.size() ||
                        index >=
                            context.object_dimensions[
                                dimension]) {
                        return fail(
                            diagnostics::implementation_index_out_of_range,
                            index_token);
                    }

                    --remaining_dimensions;
                    ++consumed_dimensions;
                }
                else {
                    const auto indexed_type =
                        value_type(current_type);

                    const auto* derived =
                        types.derived(
                            indexed_type);

                    if (!derived ||
                        derived->kind !=
                            derived_type_kind::array ||
                        index >= derived->payload) {
                        return fail(
                            diagnostics::implementation_index_out_of_range,
                            index_token);
                    }

                    current_type =
                        derived->child;
                }

                context.path_steps.push_back({
                    implementation_path_step_kind::index,
                    {},
                    index
                });

                const auto close =
                    context.tokens[
                        position - 1];

                end =
                    close.offset +
                    close.length;

                continue;
            }

            if (take(
                    implementation_punctuation::dot)) {
                if (remaining_dimensions != 0 ||
                    peek().kind !=
                        implementation_token_kind::identifier) {
                    return fail(
                        diagnostics::implementation_unknown_member,
                        peek());
                }

                const auto member_token =
                    peek();
                ++position;

                const auto owner_type =
                    value_type(current_type);

                type_handle owner;

                if (!owner_type ||
                    !types.named(
                        owner_type,
                        owner)) {
                    return fail(
                        diagnostics::implementation_unknown_member,
                        member_token);
                }

                const auto* owner_entry =
                    types.find(owner);

                if (!owner_entry ||
                    owner_entry->kind !=
                        user_type_kind::aggregate) {
                    return fail(
                        diagnostics::implementation_unknown_member,
                        member_token);
                }

                const auto member_name =
                    strings.find_for_construction(
                        text(member_token));

                const auto member =
                    member_name
                        ? types.find_member(
                              owner,
                              member_name)
                        : member_index{};

                const auto* member_record =
                    member
                        ? types.member(
                              owner,
                              member)
                        : nullptr;

                if (!member_record) {
                    return fail(
                        diagnostics::implementation_unknown_member,
                        member_token);
                }

                context.path_steps.push_back({
                    implementation_path_step_kind::member,
                    member,
                    0
                });

                current_type =
                    member_record->type;

                end =
                    member_token.offset +
                    member_token.length;

                continue;
            }

            break;
        }

        return append_path({
            object_index,
            step_offset,
            static_cast<std::uint32_t>(
                context.path_steps.size() -
                step_offset),
            remaining_dimensions,
            current_type,
            {
                root_token.offset,
                end - root_token.offset
            }
        }, path);
    }

    [[nodiscard]] char decode_escape(
        char value) const noexcept {

        switch (value) {
        case '0':
            return '\0';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case '\\':
            return '\\';
        case '\'':
            return '\'';
        case '"':
            return '"';
        default:
            return '\0';
        }
    }

    [[nodiscard]] status decode_quoted(
        const implementation_token& token,
        std::vector<char>& decoded) const {

        decoded.clear();

        if (token.length < 2) {
            return {
                status_code::configuration_failed
            };
        }

        const auto spelling =
            text(token);

        if (spelling.size() !=
            token.length) {
            return {
                status_code::configuration_failed
            };
        }

        for (std::size_t index = 1;
             index + 1 < spelling.size();
             ++index) {
            if (spelling[index] == '\\') {
                if (index + 2 >
                    spelling.size()) {
                    return {
                        status_code::configuration_failed
                    };
                }

                decoded.push_back(
                    decode_escape(
                        spelling[++index]));
            }
            else {
                decoded.push_back(
                    spelling[index]);
            }
        }

        return {};
    }

    [[nodiscard]] status append_literal(
        const implementation_token& token,
        std::uint32_t& literal) {

        literal = 0;

        if (context.literals.size() >=
            (std::numeric_limits<std::uint32_t>::max)()) {
            return {
                status_code::initialization_failed
            };
        }

        implementation_literal_fact fact;
        fact.range = {
            token.offset,
            token.length
        };

        if (token.kind ==
            implementation_token_kind::integer_literal) {
            const auto spelling =
                text(token);

            std::uint64_t value = 0;

            const auto conversion =
                std::from_chars(
                    spelling.data(),
                    spelling.data() +
                        spelling.size(),
                    value);

            if (conversion.ec != std::errc{} ||
                conversion.ptr !=
                    spelling.data() +
                        spelling.size()) {
                return fail(
                    diagnostics::implementation_invalid_source,
                    token);
            }

            fact.kind =
                implementation_literal_kind::integer;
            fact.bits = value;
        }
        else if (token.kind ==
                 implementation_token_kind::character_literal) {
            std::vector<char> decoded;

            auto result =
                decode_quoted(
                    token,
                    decoded);

            if (!result.ok() ||
                decoded.size() != 1) {
                return fail(
                    diagnostics::implementation_invalid_source,
                    token);
            }

            fact.kind =
                implementation_literal_kind::character;

            fact.bits =
                static_cast<unsigned char>(
                    decoded.front());
        }
        else if (token.kind ==
                 implementation_token_kind::string_literal) {
            std::vector<char> decoded;

            auto result =
                decode_quoted(
                    token,
                    decoded);

            if (!result.ok() ||
                context.literal_bytes.size() >
                    (std::numeric_limits<std::uint32_t>::max)() -
                        decoded.size()) {
                return fail(
                    diagnostics::implementation_invalid_source,
                    token);
            }

            fact.kind =
                implementation_literal_kind::string;

            fact.byte_offset =
                static_cast<std::uint32_t>(
                    context.literal_bytes.size());

            fact.byte_count =
                static_cast<std::uint32_t>(
                    decoded.size());

            context.literal_bytes.insert(
                context.literal_bytes.end(),
                decoded.begin(),
                decoded.end());
        }
        else {
            return fail(
                diagnostics::implementation_invalid_source,
                token);
        }

        context.literals.push_back(fact);

        literal =
            static_cast<std::uint32_t>(
                context.literals.size());

        return {};
    }

    [[nodiscard]] status parse_rhs(
        implementation_rhs& rhs) {

        rhs = {};

        if (peek().kind ==
            implementation_token_kind::identifier) {
            rhs.path = true;
            return parse_path(
                rhs.coordinate);
        }

        if (peek().kind ==
                implementation_token_kind::integer_literal ||
            peek().kind ==
                implementation_token_kind::character_literal ||
            peek().kind ==
                implementation_token_kind::string_literal) {
            const auto token = peek();
            ++position;

            return append_literal(
                token,
                rhs.coordinate);
        }

        return fail(
            diagnostics::implementation_invalid_source,
            peek());
    }

    [[nodiscard]] bool string_target_capacity(
        const implementation_path_fact& path,
        std::uint64_t& capacity) const noexcept {

        capacity = 0;

        if (!path.object_index ||
            path.object_index >
                context.objects.size()) {
            return false;
        }

        if (path.remaining_object_dimensions != 0) {
            const auto& object =
                context.objects[
                    path.object_index - 1];

            if (path.remaining_object_dimensions != 1 ||
                object.dimension_count <
                    path.remaining_object_dimensions) {
                return false;
            }

            const auto consumed =
                object.dimension_count -
                path.remaining_object_dimensions;

            const auto slot =
                static_cast<std::size_t>(
                    object.dimension_offset) +
                consumed;

            if (slot >=
                context.object_dimensions.size()) {
                return false;
            }

            builtin_type builtin;

            if (!types.builtin(
                    value_type(path.type),
                    builtin) ||
                builtin !=
                    builtin_type::character) {
                return false;
            }

            capacity =
                context.object_dimensions[slot];

            return true;
        }

        const auto array_type =
            value_type(path.type);

        const auto* derived =
            types.derived(
                array_type);

        if (!derived ||
            derived->kind !=
                derived_type_kind::array) {
            return false;
        }

        builtin_type builtin;

        if (!types.builtin(
                value_type(derived->child),
                builtin) ||
            builtin !=
                builtin_type::character) {
            return false;
        }

        capacity = derived->payload;
        return true;
    }

    [[nodiscard]] bool literal_compatible(
        const implementation_path_fact& target,
        const implementation_literal_fact& literal) const noexcept {

        if (literal.kind ==
            implementation_literal_kind::string) {
            std::uint64_t capacity = 0;

            return
                string_target_capacity(
                    target,
                    capacity) &&
                literal.byte_count <= capacity;
        }

        if (target.remaining_object_dimensions != 0) {
            return false;
        }

        const auto target_type =
            value_type(target.type);

        if (!target_type ||
            types.derived(target_type)) {
            return false;
        }

        builtin_type builtin;

        return
            types.builtin(
                target_type,
                builtin) &&
            is_integral(builtin);
    }

    [[nodiscard]] status append_operation(
        std::uint32_t target_path,
        const implementation_rhs& rhs) {

        if (!target_path ||
            target_path > context.paths.size() ||
            !rhs.coordinate) {
            return {
                status_code::configuration_failed
            };
        }

        const auto& target =
            context.paths[
                target_path - 1];

        const auto* target_derived =
            target.remaining_object_dimensions == 0
                ? types.derived(
                      target.type)
                : nullptr;

        implementation_operation_fact operation_fact;
        operation_fact.target_path =
            target_path;

        if (target_derived &&
            target_derived->kind ==
                derived_type_kind::lvalue_reference) {
            if (!rhs.path ||
                rhs.coordinate >
                    context.paths.size()) {
                return fail(
                    diagnostics::implementation_type_mismatch,
                    peek());
            }

            const auto& source_path =
                context.paths[
                    rhs.coordinate - 1];

            if (source_path.remaining_object_dimensions != 0 ||
                target_derived->child !=
                    value_type(source_path.type)) {
                return fail(
                    diagnostics::implementation_type_mismatch,
                    peek());
            }

            operation_fact.kind =
                implementation_operation_kind::binding;

            operation_fact.source_path =
                rhs.coordinate;
        }
        else if (target_derived &&
                 target_derived->kind ==
                    derived_type_kind::rvalue_reference) {
            return fail(
                diagnostics::implementation_type_mismatch,
                peek());
        }
        else if (rhs.path) {
            if (rhs.coordinate >
                    context.paths.size() ||
                target.remaining_object_dimensions != 0) {
                return fail(
                    diagnostics::implementation_type_mismatch,
                    peek());
            }

            const auto& source_path =
                context.paths[
                    rhs.coordinate - 1];

            const auto target_type =
                value_type(target.type);

            const auto source_type =
                value_type(source_path.type);

            if (!target_type ||
                !source_type ||
                source_path.remaining_object_dimensions != 0 ||
                target_type != source_type ||
                !copyable_value_type(target_type)) {
                return fail(
                    diagnostics::implementation_type_mismatch,
                    peek());
            }

            operation_fact.kind =
                implementation_operation_kind::value_copy;

            operation_fact.source_path =
                rhs.coordinate;
        }
        else {
            if (rhs.coordinate >
                context.literals.size()) {
                return {
                    status_code::configuration_failed
                };
            }

            const auto& literal =
                context.literals[
                    rhs.coordinate - 1];

            if (target_derived &&
                (target_derived->kind ==
                     derived_type_kind::lvalue_reference ||
                 target_derived->kind ==
                     derived_type_kind::rvalue_reference)) {
                return fail(
                    diagnostics::implementation_type_mismatch,
                    peek());
            }

            if (!literal_compatible(
                    target,
                    literal)) {
                return fail(
                    diagnostics::implementation_type_mismatch,
                    peek());
            }

            operation_fact.kind =
                implementation_operation_kind::value_literal;

            operation_fact.literal =
                rhs.coordinate;
        }

        context.operations.push_back(
            operation_fact);

        return {};
    }

    [[nodiscard]] status parse_declaration() {
        TypeRef type;
        source_text_range type_range;

        auto result =
            parse_type(
                type,
                type_range);

        if (!result.ok()) {
            return result;
        }

        builtin_type builtin;

        if (types.builtin(type, builtin) &&
            builtin == builtin_type::void_type) {
            return fail(
                diagnostics::implementation_type_mismatch,
                peek());
        }

        for (;;) {
            if (peek().kind !=
                implementation_token_kind::identifier) {
                return fail(
                    diagnostics::implementation_invalid_source,
                    peek());
            }

            const auto name_token = peek();
            const auto spelling =
                text(name_token);
            ++position;

            source_name_ref name;

            result =
                context.store_name(
                    spelling,
                    name);

            if (!result.ok()) {
                return result;
            }

            implementation_object_fact fact;
            fact.name = name;
            fact.type = type;
            fact.dimension_offset =
                static_cast<std::uint32_t>(
                    context.object_dimensions.size());
            fact.name_range = {
                name_token.offset,
                name_token.length
            };
            fact.type_range =
                type_range;

            auto declaration_end =
                name_token.offset +
                name_token.length;

            while (take(
                    implementation_punctuation::left_bracket)) {
                std::uint64_t extent = 0;

                result =
                    parse_integer(
                        extent,
                        true);

                if (!result.ok()) {
                    return result;
                }

                if (!take(
                        implementation_punctuation::right_bracket)) {
                    return fail(
                        diagnostics::implementation_invalid_source,
                        peek());
                }

                context.object_dimensions.push_back(
                    extent);

                ++fact.dimension_count;

                const auto close =
                    context.tokens[
                        position - 1];

                declaration_end =
                    close.offset +
                    close.length;
            }

            fact.declaration_range = {
                type_range.offset,
                declaration_end -
                    type_range.offset
            };

            std::uint32_t object = 0;

            result =
                context.declare_object(
                    spelling,
                    fact,
                    object);

            if (!result.ok()) {
                if (result.code ==
                    status_code::initialization_failed) {
                    return result;
                }

                return fail(
                    diagnostics::implementation_duplicate_object,
                    name_token);
            }

            if (take(
                    implementation_punctuation::equal)) {
                std::uint32_t target = 0;

                result =
                    make_root_path(
                        object,
                        target);

                if (!result.ok()) {
                    return result;
                }

                implementation_rhs rhs;

                result =
                    parse_rhs(rhs);

                if (!result.ok()) {
                    return result;
                }

                result =
                    append_operation(
                        target,
                        rhs);

                if (!result.ok()) {
                    return result;
                }
            }

            if (take(
                    implementation_punctuation::comma)) {
                continue;
            }

            if (!take(
                    implementation_punctuation::semicolon)) {
                return fail(
                    diagnostics::implementation_invalid_source,
                    peek());
            }

            return {};
        }
    }

    [[nodiscard]] status parse_assignment() {
        std::uint32_t target = 0;

        auto result =
            parse_path(target);

        if (!result.ok()) {
            return result;
        }

        if (!take(
                implementation_punctuation::equal)) {
            return fail(
                diagnostics::implementation_invalid_source,
                peek());
        }

        implementation_rhs rhs;

        result =
            parse_rhs(rhs);

        if (!result.ok()) {
            return result;
        }

        result =
            append_operation(
                target,
                rhs);

        if (!result.ok()) {
            return result;
        }

        if (!take(
                implementation_punctuation::semicolon)) {
            return fail(
                diagnostics::implementation_invalid_source,
                peek());
        }

        return {};
    }

    [[nodiscard]] status parse_statement() {
        if (peek().kind !=
            implementation_token_kind::identifier) {
            return fail(
                diagnostics::implementation_invalid_source,
                peek());
        }

        return declaration_ahead()
            ? parse_declaration()
            : parse_assignment();
    }

    source_view source;
    const graph_type_view& types;
    const string_registry& strings;
    operation_id operation;
    implementation_context& context;
    std::size_t position = 0;
};

status emit_initialization_failure(
    source_view source,
    operation_id operation,
    implementation_context& context) noexcept {

    try {
        context.diagnostics.emit({
            diagnostics::implementation_initialization_failed.id,
            diagnostics::implementation_initialization_failed.default_severity,
            operation,
            {source.source, 0, 0},
            {}
        });

        return {
            status_code::initialization_failed
        };
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

} // namespace

status parse_implementation_source(
    source_view source,
    const graph_type_view& types,
    const string_registry& strings,
    operation_id operation,
    implementation_context& context) noexcept {

    context.reset();

    auto result =
        lex_implementation_source(
            source,
            operation,
            context.diagnostics,
            context.tokens);

    if (!result.ok()) {
        return result;
    }

    if (context.tokens.empty()) {
        return {
            status_code::initialization_failed
        };
    }

    try {
        implementation_parser parser{
            source,
            types,
            strings,
            operation,
            context
        };

        return parser.parse();
    }
    catch (...) {
        return emit_initialization_failure(
            source,
            operation,
            context);
    }
}

} // namespace cw::server
