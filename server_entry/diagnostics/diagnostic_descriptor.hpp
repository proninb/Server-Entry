#pragma once

#include "diagnostic.hpp"

#include <string_view>

namespace cw::server
{

struct diagnostic_descriptor
{
    diagnostic_id id;
    diagnostic_domain domain = diagnostic_domain::unknown;
    diagnostic_severity default_severity = diagnostic_severity::error;
    std::string_view name;
    std::string_view message;
};

namespace diagnostics
{

inline constexpr diagnostic_descriptor server_initialization_failed{
    diagnostic_id{1001}, diagnostic_domain::server, diagnostic_severity::fatal,
    "server.initialization_failed", "Server initialization failed"};

inline constexpr diagnostic_descriptor server_invalid_json{
    diagnostic_id{1002}, diagnostic_domain::server, diagnostic_severity::error,
    "server.invalid_json", "Server configuration contains invalid JSON"};
inline constexpr diagnostic_descriptor server_invalid_configuration{
    diagnostic_id{1003}, diagnostic_domain::server, diagnostic_severity::error,
    "server.invalid_configuration", "Server configuration is invalid"};
inline constexpr diagnostic_descriptor server_unsupported_configuration_version{
    diagnostic_id{1004}, diagnostic_domain::server, diagnostic_severity::error,
    "server.unsupported_configuration_version", "Server configuration version is unsupported"};
inline constexpr diagnostic_descriptor server_configuration_read_failed{
    diagnostic_id{1005}, diagnostic_domain::server, diagnostic_severity::error,
    "server.configuration_read_failed", "Server configuration could not be read"};

inline constexpr diagnostic_descriptor project_initialization_failed{
    diagnostic_id{2001}, diagnostic_domain::project, diagnostic_severity::error,
    "project.initialization_failed", "Project initialization failed"};
inline constexpr diagnostic_descriptor project_invalid_json{
    diagnostic_id{2002}, diagnostic_domain::project, diagnostic_severity::error,
    "project.invalid_json", "Project configuration contains invalid JSON"};
inline constexpr diagnostic_descriptor project_invalid_configuration{
    diagnostic_id{2003}, diagnostic_domain::project, diagnostic_severity::error,
    "project.invalid_configuration", "Project configuration is invalid"};
inline constexpr diagnostic_descriptor project_unsupported_configuration_version{
    diagnostic_id{2004}, diagnostic_domain::project, diagnostic_severity::error,
    "project.unsupported_configuration_version", "Project configuration version is unsupported"};
inline constexpr diagnostic_descriptor project_configuration_read_failed{
    diagnostic_id{2005}, diagnostic_domain::project, diagnostic_severity::error,
    "project.configuration_read_failed", "Project configuration could not be read"};
inline constexpr diagnostic_descriptor project_composition_cycle{
    diagnostic_id{2006}, diagnostic_domain::project, diagnostic_severity::error,
    "project.composition_cycle", "Project composition contains a cycle"};
inline constexpr diagnostic_descriptor project_composition_failed{
    diagnostic_id{2007}, diagnostic_domain::project, diagnostic_severity::error,
    "project.composition_failed", "Project composition could not be resolved"};

inline constexpr diagnostic_descriptor source_initialization_failed{
    diagnostic_id{3001}, diagnostic_domain::source, diagnostic_severity::error,
    "source.initialization_failed", "Source Manager initialization failed"};
inline constexpr diagnostic_descriptor source_include_cycle{
    diagnostic_id{3002}, diagnostic_domain::source, diagnostic_severity::error,
    "source.include_cycle", "Source include graph contains a cycle"};
inline constexpr diagnostic_descriptor source_include_not_found{
    diagnostic_id{3003}, diagnostic_domain::source, diagnostic_severity::error,
    "source.include_not_found", "Included Source could not be resolved"};
inline constexpr diagnostic_descriptor source_unsupported_directive{
    diagnostic_id{3004}, diagnostic_domain::source, diagnostic_severity::error,
    "source.unsupported_directive", "Preprocessor directive is not supported"};
inline constexpr diagnostic_descriptor source_include_inside_namespace{
    diagnostic_id{3005}, diagnostic_domain::source, diagnostic_severity::error,
    "source.include_inside_namespace",
    "Include directives inside namespace scope are not supported"};
inline constexpr diagnostic_descriptor source_acquisition_failed{
    diagnostic_id{3006}, diagnostic_domain::source, diagnostic_severity::error,
    "source.acquisition_failed", "Source file could not be acquired"};
inline constexpr diagnostic_descriptor source_checkpoint_save_failed{
    diagnostic_id{3007}, diagnostic_domain::source, diagnostic_severity::error,
    "source.checkpoint_save_failed", "Source checkpoint could not be saved"};

inline constexpr diagnostic_descriptor builder_duplicate_source_replacement{
    diagnostic_id{5001}, diagnostic_domain::builder, diagnostic_severity::error,
    "builder.duplicate_source_replacement",
    "Source has more than one replacement batch in the build operation"};
inline constexpr diagnostic_descriptor parser_invalid_source{
    diagnostic_id{4001}, diagnostic_domain::parser, diagnostic_severity::error,
    "parser.invalid_source", "Source contains an invalid or unsupported declaration"};
inline constexpr diagnostic_descriptor parser_invalid_enum_forward_declaration{
    diagnostic_id{4002}, diagnostic_domain::parser, diagnostic_severity::error,
    "parser.invalid_enum_forward_declaration",
    "An unscoped opaque enum requires an explicit underlying type"};
inline constexpr diagnostic_descriptor parser_anonymous_opaque_enum{
    diagnostic_id{4003}, diagnostic_domain::parser, diagnostic_severity::error,
    "parser.anonymous_opaque_enum", "An opaque enum must have a name"};
inline constexpr diagnostic_descriptor parser_invalid_enum_underlying{
    diagnostic_id{4004}, diagnostic_domain::parser, diagnostic_severity::error,
    "parser.invalid_enum_underlying", "Enum underlying type is invalid"};
inline constexpr diagnostic_descriptor parser_expected_enumerator_identifier{
    diagnostic_id{4005}, diagnostic_domain::parser, diagnostic_severity::error,
    "parser.expected_enumerator_identifier", "Expected an enumerator identifier"};
inline constexpr diagnostic_descriptor parser_expected_semicolon{
    diagnostic_id{4006}, diagnostic_domain::parser, diagnostic_severity::error,
    "parser.expected_semicolon", "Expected a semicolon after enum declaration"};
inline constexpr diagnostic_descriptor parser_invalid_enumerator_expression{
    diagnostic_id{4007}, diagnostic_domain::parser, diagnostic_severity::error,
    "parser.invalid_enumerator_expression", "Enumerator integral expression is invalid"};
inline constexpr diagnostic_descriptor parser_duplicate_enumerator{
    diagnostic_id{4008}, diagnostic_domain::parser, diagnostic_severity::error,
    "parser.duplicate_enumerator", "Enumerator name is duplicated in this enum"};
inline constexpr diagnostic_descriptor parser_initialization_failed{
    diagnostic_id{4009}, diagnostic_domain::parser, diagnostic_severity::fatal,
    "parser.initialization_failed", "Parser could not complete because an internal resource failed"};
inline constexpr diagnostic_descriptor builder_invalid_source_fact{
    diagnostic_id{5002}, diagnostic_domain::builder, diagnostic_severity::error,
    "builder.invalid_source_fact", "Parser produced an invalid transient source fact"};
inline constexpr diagnostic_descriptor builder_semantic_failure{
    diagnostic_id{5003}, diagnostic_domain::builder, diagnostic_severity::error,
    "builder.semantic_failure", "Canonical declaration conflicts with project semantics"};
inline constexpr diagnostic_descriptor canonicalization_failed{
    diagnostic_id{5004}, diagnostic_domain::builder, diagnostic_severity::error,
    "canonicalization.failed", "Source identity could not be admitted to canonical state"};
inline constexpr diagnostic_descriptor construction_initialization_failed{
    diagnostic_id{5005}, diagnostic_domain::builder, diagnostic_severity::fatal,
    "construction.initialization_failed",
    "Canonical construction could not complete because an internal resource failed"};

inline constexpr diagnostic_descriptor runtime_attach_failed{
    diagnostic_id{6001}, diagnostic_domain::runtime, diagnostic_severity::error,
    "runtime.attach_failed", "Runtime attachment failed"};

inline constexpr diagnostic_descriptor shm_initialization_failed{
    diagnostic_id{7001}, diagnostic_domain::shm, diagnostic_severity::error,
    "shm.initialization_failed", "Shared Memory initialization failed"};

} // namespace diagnostics
} // namespace cw::server
