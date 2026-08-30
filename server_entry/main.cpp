#include "config/server_configuration_loader.hpp"
#include "server_context.hpp"

#include <filesystem>
#include <iostream>
#include <utility>

int main(int argc, char* argv[])
{
    const std::filesystem::path configuration_path = argc > 1 ? argv[1] : "server.json";
    cw::server::server_configuration configuration;
    cw::server::diagnostic_buffer diagnostics;

    const auto configuration_result = cw::server::load_server_configuration_file(
        configuration_path, cw::server::operation_id{}, diagnostics, configuration);
    if (!configuration_result.ok())
    {
        std::cerr << "Failed to load server configuration: " << configuration_path << '\n';
        return 1;
    }

    cw::server::server_context server{std::move(configuration)};

    const auto result = server.initialize();
    if (!result.ok())
    {
        return 1;
    }

    server.shutdown();
    return 0;
}
