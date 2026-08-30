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

inline constexpr diagnostic_descriptor source_initialization_failed{
    diagnostic_id{3001}, diagnostic_domain::source, diagnostic_severity::error,
    "source.initialization_failed", "Source Manager initialization failed"};

inline constexpr diagnostic_descriptor runtime_attach_failed{
    diagnostic_id{6001}, diagnostic_domain::runtime, diagnostic_severity::error,
    "runtime.attach_failed", "Runtime attachment failed"};

inline constexpr diagnostic_descriptor shm_initialization_failed{
    diagnostic_id{7001}, diagnostic_domain::shm, diagnostic_severity::error,
    "shm.initialization_failed", "Shared Memory initialization failed"};

} // namespace diagnostics
} // namespace cw::server
