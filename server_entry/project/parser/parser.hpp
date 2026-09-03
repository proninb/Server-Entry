#pragma once
#include "source_context.hpp"
#include "source_environment.hpp"
#include "token.hpp"
#include "language_configuration.hpp"
#include "../source/source_manager.hpp"
#include "../../operation.hpp"
namespace cw::server {
[[nodiscard]] status parse_source(source_view,const source_environment&,operation_id,source_context&)noexcept;
[[nodiscard]] status parse_source_tokens(source_view,std::span<const parser_token>,
    const source_environment&,operation_id,source_context&)noexcept;

class parser_backend
{
public:
    virtual ~parser_backend() = default;
    [[nodiscard]] virtual status parse(
        source_view source, std::span<const parser_token> tokens,
        const source_environment& environment,
        const language_configuration& language, operation_id operation,
        source_context& context) const noexcept = 0;
};

class native_parser_backend final : public parser_backend
{
public:
    [[nodiscard]] status parse(
        source_view source, std::span<const parser_token> tokens,
        const source_environment& environment,
        const language_configuration& language, operation_id operation,
        source_context& context) const noexcept override;
};

[[nodiscard]] const parser_backend& default_parser_backend() noexcept;
}
