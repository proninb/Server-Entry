#include "parser.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"
#include "lexer.hpp"
#include <charconv>
#include <limits>
#include <string>
#include <unordered_map>
namespace cw::server {
namespace {
class parser final {
public:
  parser(source_view s, const source_environment &e, operation_id op,
         source_context &out, std::span<const parser_token> tokens) noexcept
      : source_(s), environment_(e), operation_(op), out_(out),
        tokens_(tokens) {}
  status unit(bool nested = false) noexcept {
    try {
      while (
          peek().kind != parser_token_kind::eof &&
          !(nested && peek().punctuation == parser_punctuation::right_brace)) {
        if (peek().kind == parser_token_kind::keyword_enum) {
          auto r = parse_enum();
          if (!r.ok())
            return r;
        } else if (peek().kind == parser_token_kind::keyword_struct) {
          auto r = parse_struct();
          if (!r.ok())
            return r;
        } else if (peek().kind == parser_token_kind::keyword_namespace) {
          auto r = parse_namespace();
          if (!r.ok())
            return r;
        } else
          return fail(diagnostics::parser_invalid_source, peek().offset,
                      peek().length);
      }
      return {};
    } catch (...) {
      return {status_code::initialization_failed};
    }
  }

private:
  static std::uint32_t n(std::size_t x) noexcept {
    return static_cast<std::uint32_t>(x);
  }
  std::string_view text(const parser_token &token) const noexcept {
    return source_.bytes.substr(token.offset, token.length);
  }
  status fail(const diagnostic_descriptor &d, std::uint32_t o,
              std::uint32_t l) noexcept {
    try {
      out_.diagnostics.emit(
          {d.id, d.default_severity, operation_, {source_.source, o, l}, {}});
      return {status_code::configuration_failed};
    } catch (...) {
      return {status_code::initialization_failed};
    }
  }
  const parser_token &peek() const noexcept { return tokens_[position_]; }
  bool take(parser_punctuation p) noexcept {
    if (peek().punctuation != p)
      return false;
    ++position_;
    return true;
  }
  bool take(parser_token_kind k) noexcept {
    if (peek().kind != k)
      return false;
    ++position_;
    return true;
  }
  status store_qualified(std::string_view local,
                         source_name_ref &out) noexcept {
    try {
      std::string q;
      if (!scope_.empty()) {
        q = scope_;
        q += "::";
      }
      q += local;
      return out_.store_name(q, out);
    } catch (...) {
      return {status_code::initialization_failed};
    }
  }
  status parse_underlying(std::optional<builtin_type> &out,
                          source_text_range &range) noexcept {
    if (peek().kind != parser_token_kind::identifier)
      return fail(diagnostics::parser_invalid_enum_underlying, peek().offset,
                  peek().length);
    auto first = peek();
    range = {first.offset, first.length};
    ++position_;
    if (text(first) == "char")
      out = builtin_type::character;
    else if (text(first) == "short")
      out = builtin_type::short_integer;
    else if (text(first) == "int")
      out = builtin_type::integer;
    else if (text(first) == "long") {
      if (peek().kind == parser_token_kind::identifier &&
          text(peek()) == "long") {
        range.length = peek().offset + peek().length - range.offset;
        ++position_;
        out = builtin_type::long_long_integer;
      } else
        out = builtin_type::long_integer;
    } else if (text(first) == "unsigned") {
      if (peek().kind != parser_token_kind::identifier)
        out = builtin_type::unsigned_integer;
      else if (text(peek()) == "char") {
        range.length = peek().offset + peek().length - range.offset;
        ++position_;
        out = builtin_type::unsigned_character;
      } else if (text(peek()) == "short") {
        range.length = peek().offset + peek().length - range.offset;
        ++position_;
        out = builtin_type::unsigned_short_integer;
      } else if (text(peek()) == "int") {
        range.length = peek().offset + peek().length - range.offset;
        ++position_;
        out = builtin_type::unsigned_integer;
      } else if (text(peek()) == "long") {
        range.length = peek().offset + peek().length - range.offset;
        ++position_;
        if (peek().kind == parser_token_kind::identifier &&
            text(peek()) == "long") {
          range.length = peek().offset + peek().length - range.offset;
          ++position_;
          out = builtin_type::unsigned_long_long_integer;
        } else
          out = builtin_type::unsigned_long_integer;
      } else
        return fail(diagnostics::parser_invalid_enum_underlying, peek().offset,
                    peek().length);
    } else
      return fail(diagnostics::parser_invalid_enum_underlying, first.offset,
                  first.length);
    return {};
  }
  status number(integral_constant &out) noexcept {
    bool negative = take(parser_punctuation::minus);
    if (peek().kind != parser_token_kind::integer_literal)
      return fail(diagnostics::parser_invalid_enumerator_expression,
                  peek().offset, peek().length);
    auto t = peek();
    ++position_;
    auto spelling = text(t);
    std::uint64_t value = 0;
    auto r = std::from_chars(spelling.data(), spelling.data() + spelling.size(),
                             value);
    if (r.ec != std::errc{} || r.ptr != spelling.data() + spelling.size())
      return fail(diagnostics::parser_invalid_enumerator_expression, t.offset,
                  t.length);
    if (negative) {
      if (value > (std::uint64_t{1} << 63))
        return fail(diagnostics::parser_invalid_enumerator_expression, t.offset,
                    t.length);
      out = {builtin_type::long_long_integer, std::uint64_t{0} - value};
    } else
      out = {value <= static_cast<std::uint64_t>(
                          (std::numeric_limits<std::int64_t>::max)())
                 ? builtin_type::long_long_integer
                 : builtin_type::unsigned_long_long_integer,
             value};
    return {};
  }
  bool lookup(std::string_view name, integral_constant &out) const noexcept {
    auto local = locals_.find(name);
    if (local != locals_.end()) {
      out = local->second;
      return true;
    }
    auto scope = std::string_view{scope_};
    for (;;) {
      if (environment_.find_constant_exact(scope, name, out).ok())
        return true;
      if (scope.empty())
        break;
      auto parent = scope.rfind("::");
      scope = parent == std::string_view::npos ? std::string_view{}
                                               : scope.substr(0, parent);
    }
    return false;
  }
  status primary(integral_constant &out) noexcept {
    if (peek().kind == parser_token_kind::integer_literal ||
        peek().punctuation == parser_punctuation::minus)
      return number(out);
    if (peek().kind == parser_token_kind::identifier) {
      auto t = peek();
      ++position_;
      if (lookup(text(t), out))
        return {};
      return fail(diagnostics::parser_invalid_enumerator_expression, t.offset,
                  t.length);
    }
    return fail(diagnostics::parser_invalid_enumerator_expression,
                peek().offset, peek().length);
  }
  status expression(integral_constant &out) noexcept {
    auto r = primary(out);
    if (!r.ok())
      return r;
    while (take(parser_punctuation::plus)) {
      integral_constant right;
      r = primary(right);
      if (!r.ok())
        return r;
      bool uns = out.type == builtin_type::unsigned_long_long_integer ||
                 right.type == builtin_type::unsigned_long_long_integer;
      if (uns) {
        if ((std::numeric_limits<std::uint64_t>::max)() - out.bits < right.bits)
          return fail(diagnostics::parser_invalid_enumerator_expression,
                      peek().offset, peek().length);
        out = {builtin_type::unsigned_long_long_integer, out.bits + right.bits};
      } else {
        auto a = static_cast<std::int64_t>(out.bits),
             b = static_cast<std::int64_t>(right.bits);
        if ((b > 0 && a > (std::numeric_limits<std::int64_t>::max)() - b) ||
            (b < 0 && a < (std::numeric_limits<std::int64_t>::min)() - b))
          return fail(diagnostics::parser_invalid_enumerator_expression,
                      peek().offset, peek().length);
        out = {builtin_type::long_long_integer,
               static_cast<std::uint64_t>(a + b)};
      }
    }
    return {};
  }
  status increment(integral_constant &v, const parser_token &at) noexcept {
    if (v.type == builtin_type::unsigned_long_long_integer) {
      if (v.bits == (std::numeric_limits<std::uint64_t>::max)())
        return fail(diagnostics::parser_invalid_enumerator_expression,
                    at.offset, at.length);
    } else if (v.bits == static_cast<std::uint64_t>(
                             (std::numeric_limits<std::int64_t>::max)()))
      return fail(diagnostics::parser_invalid_enumerator_expression, at.offset,
                  at.length);
    ++v.bits;
    return {};
  }
  status parse_enum() noexcept {
    auto declaration = peek();
    ++position_;
    bool scoped = take(parser_token_kind::keyword_class);
    parser_token name_token{};
    if (peek().kind == parser_token_kind::identifier) {
      name_token = peek();
      ++position_;
    }
    bool anonymous = name_token.length == 0;
    std::optional<builtin_type> base;
    source_text_range base_range;
    if (take(parser_punctuation::colon)) {
      auto r = parse_underlying(base, base_range);
      if (!r.ok())
        return r;
    }
    enum_declaration_source_fact fact;
    fact.anonymous = anonymous;
    fact.scoped = scoped;
    fact.explicit_underlying = base;
    fact.declaration_range = {declaration.offset, 0};
    fact.name_range = {name_token.offset, name_token.length};
    fact.underlying_range = base_range;
    if (!scope_.empty()) {
      auto r = out_.store_name(scope_, fact.scope_name);
      if (!r.ok())
        return r;
    }
    if (!anonymous) {
      auto r = store_qualified(text(name_token), fact.canonical_name);
      if (!r.ok())
        return r;
    }
    fact.enumerator_offset = n(out_.enum_values.size());
    if (take(parser_punctuation::semicolon)) {
      if (anonymous)
        return fail(diagnostics::parser_anonymous_opaque_enum,
                    declaration.offset, declaration.length);
      if (!scoped && !base)
        return fail(diagnostics::parser_invalid_enum_forward_declaration,
                    declaration.offset, declaration.length);
      fact.definition_state = enum_definition_state::opaque;
      fact.declaration_range.length =
          peek().offset - fact.declaration_range.offset;
      out_.enums.push_back(fact);
      return {};
    }
    if (!take(parser_punctuation::left_brace))
      return fail(diagnostics::parser_invalid_source, peek().offset,
                  peek().length);
    locals_.clear();
    integral_constant next{builtin_type::long_long_integer, 0};
    while (!take(parser_punctuation::right_brace)) {
      if (peek().kind != parser_token_kind::identifier)
        return fail(diagnostics::parser_expected_enumerator_identifier,
                    peek().offset, peek().length);
      auto nt = peek();
      ++position_;
      if (locals_.contains(text(nt)))
        return fail(diagnostics::parser_duplicate_enumerator, nt.offset,
                    nt.length);
      source_name_ref name;
      auto r = out_.store_name(text(nt), name);
      if (!r.ok())
        return r;
      integral_constant value = next;
      source_text_range expr{nt.offset, nt.length};
      if (take(parser_punctuation::equal)) {
        auto start = peek().offset;
        r = expression(value);
        if (!r.ok())
          return r;
        expr = {start, peek().offset - start};
      }
      locals_.emplace(text(nt), value);
      out_.enum_values.push_back({name, value, {nt.offset, nt.length}, expr});
      ++fact.enumerator_count;
      if (!take(parser_punctuation::comma)) {
        if (peek().punctuation != parser_punctuation::right_brace)
          return fail(diagnostics::parser_invalid_source, peek().offset,
                      peek().length);
        continue;
      }
      if (peek().punctuation == parser_punctuation::right_brace)
        continue;
      next = value;
      r = increment(next, nt);
      if (!r.ok())
        return r;
    }
    if (!take(parser_punctuation::semicolon))
      return fail(diagnostics::parser_expected_semicolon, peek().offset,
                  peek().length);
    fact.declaration_range.length =
        peek().offset - fact.declaration_range.offset;
    out_.enums.push_back(fact);
    return {};
  }
  status parse_namespace() noexcept {
    auto at = peek();
    ++position_;
    if (peek().kind != parser_token_kind::identifier)
      return fail(diagnostics::parser_invalid_source, at.offset, at.length);
    auto old = scope_.size();
    if (!scope_.empty())
      scope_ += "::";
    scope_ += text(peek());
    ++position_;
    if (!take(parser_punctuation::left_brace))
      return fail(diagnostics::parser_invalid_source, peek().offset,
                  peek().length);
    auto r = unit(true);
    if (!r.ok())
      return r;
    if (!take(parser_punctuation::right_brace))
      return fail(diagnostics::parser_invalid_source, peek().offset,
                  peek().length);
    scope_.resize(old);
    return {};
  }
  status parse_struct() noexcept {
    const auto declaration = peek();
    ++position_;
    if (peek().kind != parser_token_kind::identifier)
      return fail(diagnostics::parser_invalid_source, peek().offset,
                  peek().length);
    const auto name = peek();
    ++position_;
    aggregate_declaration_source_fact fact;
    fact.name_range = {name.offset, name.length};
    fact.declaration_range = {declaration.offset, 0};
    auto result = store_qualified(text(name), fact.canonical_name);
    if (!result.ok()) return result;
    if (!scope_.empty()) {
      result = out_.store_name(scope_, fact.scope_name);
      if (!result.ok()) return result;
    }
    if (take(parser_punctuation::semicolon)) {
      fact.definition_state = aggregate_definition_state::declared;
    } else {
      if (!take(parser_punctuation::left_brace))
        return fail(diagnostics::parser_invalid_source, peek().offset,
                    peek().length);
      fact.member_offset = static_cast<std::uint32_t>(out_.aggregate_members.size());
      while (!take(parser_punctuation::right_brace)) {
        const auto member_start = peek();
        if (member_start.kind != parser_token_kind::identifier)
          return fail(diagnostics::parser_invalid_source, member_start.offset,
                      member_start.length);
        ++position_;
        member_declaration_source_fact member;
        member.type_range = {member_start.offset, member_start.length};
        if (text(member_start) == "int")
          member.builtin = builtin_type::integer;
        else {
          auto type_name = std::string{text(member_start)};
          if (!scope_.empty()) type_name = scope_ + "::" + type_name;
          result = out_.store_name(type_name, member.type_name);
          if (!result.ok()) return result;
        }
        member.lvalue_reference = take(parser_punctuation::ampersand);
        if (peek().kind != parser_token_kind::identifier)
          return fail(diagnostics::parser_invalid_source, peek().offset,
                      peek().length);
        const auto member_name = peek();
        ++position_;
        result = out_.store_name(text(member_name), member.name);
        if (!result.ok()) return result;
        member.name_range = {member_name.offset, member_name.length};
        if (!take(parser_punctuation::semicolon))
          return fail(diagnostics::parser_expected_semicolon, peek().offset,
                      peek().length);
        member.declaration_range = {member_start.offset,
          peek().offset - member_start.offset};
        out_.aggregate_members.push_back(member);
        ++fact.member_count;
      }
      if (!take(parser_punctuation::semicolon))
        return fail(diagnostics::parser_expected_semicolon, peek().offset,
                    peek().length);
      fact.definition_state = aggregate_definition_state::defined;
    }
    fact.declaration_range.length = peek().offset - declaration.offset;
    out_.aggregates.push_back(fact);
    return {};
  }
  source_view source_;
  const source_environment &environment_;
  operation_id operation_;
  source_context &out_;
  std::span<const parser_token> tokens_;
  std::size_t position_ = 0;
  std::string scope_;
  std::unordered_map<std::string_view, integral_constant> locals_;
};
} // namespace
status parse_source_tokens(source_view source,
                           std::span<const parser_token> tokens,
                           const source_environment &environment,
                           operation_id operation,
                           source_context &context) noexcept {
  context.reset();
  status result;
  if (tokens.empty() || tokens.back().kind != parser_token_kind::eof)
    result = {status_code::configuration_failed};
  else {
    parser p{source, environment, operation, context, tokens};
    result = p.unit();
  }
  if (result.code == status_code::initialization_failed &&
      context.diagnostics.empty())
    try {
      context.diagnostics.emit(
          {diagnostics::parser_initialization_failed.id,
           diagnostics::parser_initialization_failed.default_severity,
           operation,
           {source.source, 0, 0},
           {}});
    } catch (...) { /* status is the unavoidable fallback when diagnostic
                       storage itself failed */
    }
  return result;
}

status parse_source(source_view source, const source_environment &environment,
                    operation_id operation, source_context &context) noexcept {
  context.reset();
  auto result =
      lex_source(source, operation, context.diagnostics, context.tokens);
  if (!result.ok())
    return result;
  parser p{source, environment, operation, context, context.tokens};
  result = p.unit();
  if (result.code == status_code::initialization_failed &&
      context.diagnostics.empty())
    try {
      context.diagnostics.emit(
          {diagnostics::parser_initialization_failed.id,
           diagnostics::parser_initialization_failed.default_severity,
           operation,
           {source.source, 0, 0},
           {}});
    } catch (...) { /* status is the unavoidable fallback when diagnostic
                       storage itself failed */
    }
  return result;
}

status native_parser_backend::parse(
    source_view source, std::span<const parser_token> tokens,
    const source_environment& environment,
    const language_configuration& language, operation_id operation,
    source_context& context) const noexcept
{
    (void)language;
    return parse_source_tokens(source, tokens, environment, operation, context);
}

const parser_backend& default_parser_backend() noexcept
{
    static const native_parser_backend backend;
    return backend;
}
} // namespace cw::server
