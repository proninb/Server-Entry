#pragma once

#include "../logging/log.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cw::server {

inline constexpr std::uint32_t current_server_configuration_version = 1;

// Selects the transport used by a configured communication endpoint.
enum class transport_kind : std::uint8_t {
    tcp,
};

// Selects the application protocol carried by a communication endpoint.
enum class protocol_kind : std::uint8_t {
    json,
};

// Describes one externally reachable Server communication endpoint.
// The endpoint binds a named transport/protocol pair to a network address and port.
struct endpoint_configuration {
    std::string name;
    transport_kind transport = transport_kind::tcp;
    protocol_kind protocol = protocol_kind::json;
    std::string address;
    std::uint16_t port = 0;
};

// Defines the Server communication surface.
// It owns endpoint configuration only; connection lifetime and protocol execution
// belong to the communication runtime.
struct communication_configuration {
    std::vector<endpoint_configuration> endpoints;

    // Local interactive command endpoint; unrelated to the console log sink.
    bool console = true;
};

// Defines process-wide logging policy used when server_context configures logging.
struct logging_configuration {
    log_level minimum_level = log_level::info;

    // Controls terminal log output; unrelated to the local command endpoint.
    bool console = true;
};

// Defines optional process-wide telemetry facilities.
struct telemetry_configuration {
    bool metrics = true;
};

// Identifies the project that this Server process owns and loads.
struct project_reference {
    std::filesystem::path path;
};

// Root configuration for one Server process.
// It contains process-level communication, logging, telemetry, and the single
// project reference consumed by server_context during Server startup.
struct server_configuration {
    std::uint32_t version = 0;
    communication_configuration communication;
    logging_configuration logging;
    telemetry_configuration telemetry;
    project_reference project;
};

} // namespace cw::server
