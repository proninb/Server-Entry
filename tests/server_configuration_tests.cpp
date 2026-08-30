#include "../server_entry/config/server_configuration_loader.hpp"
#include "../server_entry/diagnostics/diagnostic_descriptor.hpp"

#include <filesystem>
#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace
{

using namespace cw::server;

constexpr std::string_view full_configuration = R"({
  "version": 1,
  "server": {
    "endpoints": [{
      "name": "main",
      "transport": "tcp",
      "protocol": "json",
      "address": "0.0.0.0",
      "port": 39001
    }],
    "console": true
  },
  "logging": {"level": "info", "console": true},
  "telemetry": {"metrics": true},
  "project": {"path": "../project/project.json"}
})";

static_assert(noexcept(load_server_configuration(
    std::declval<std::string_view>(), std::declval<const std::filesystem::path&>(),
    std::declval<operation_id>(), std::declval<diagnostic_buffer&>(),
    std::declval<server_configuration&>())));
static_assert(current_server_configuration_version == 1);

bool load(std::string_view text, server_configuration& output, diagnostic_buffer& diagnostics,
          std::filesystem::path path = "C:/Server/config/server.json")
{
    return load_server_configuration(text, path, operation_id{12}, diagnostics, output).ok();
}

bool rejects(std::string_view text)
{
    server_configuration output;
    diagnostic_buffer diagnostics;
    return !load(text, output, diagnostics) && !diagnostics.empty();
}

bool test_valid_full_and_paths()
{
    server_configuration output;
    diagnostic_buffer diagnostics;
    if (!load(full_configuration, output, diagnostics) || output.version != 1 ||
        output.communication.endpoints.size() != 1 ||
        output.communication.endpoints[0].name != "main" ||
        output.communication.endpoints[0].port != 39001 ||
        !output.communication.console || !output.logging.console ||
        !output.telemetry.metrics || output.logging.minimum_level != log_level::info)
        return false;

    if (output.project.path != std::filesystem::path{"C:/Server/project/project.json"})
        return false;

    constexpr std::string_view absolute = R"({"version":1,"server":{"endpoints":[],"console":false},"logging":{"level":"critical","console":false},"telemetry":{"metrics":false},"project":{"path":"D:/Projects/A/project.json"}})";
    diagnostics.clear();
    if (!load(absolute, output, diagnostics) || output.communication.console ||
        output.logging.console || output.telemetry.metrics ||
        output.logging.minimum_level != log_level::critical ||
        output.project.path != std::filesystem::path{"D:/Projects/A/project.json"})
        return false;
    return output.communication.endpoints.empty();
}

bool test_valid_variants()
{
    constexpr std::string_view multiple = R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"tcp","protocol":"json","address":"127.0.0.1","port":1},{"name":"b","transport":"tcp","protocol":"json","address":"0.0.0.0","port":65535}],"console":false},"logging":{"level":"debug","console":true},"telemetry":{"metrics":false},"project":{"path":"p.json"}})";
    server_configuration output;
    diagnostic_buffer diagnostics;
    if (!load(multiple, output, diagnostics) || output.communication.endpoints.size() != 2 ||
        output.communication.console || output.telemetry.metrics) return false;

    constexpr std::array levels{"trace", "debug", "info", "warning", "error", "critical"};
    for (const auto* level : levels)
    {
        std::string text = "{\"version\":1,\"server\":{\"endpoints\":[],\"console\":true},"
                           "\"logging\":{\"level\":\"";
        text += level;
        text += "\",\"console\":true},\"telemetry\":{\"metrics\":true},"
                "\"project\":{\"path\":\"p.json\"}}";
        diagnostics.clear();
        if (!load(text, output, diagnostics)) return false;
    }
    return true;
}

bool test_syntax_version_and_required()
{
    if (!rejects("{\"version\":1,")) return false;

    server_configuration output;
    diagnostic_buffer diagnostics;
    if (load(R"({"version":2,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})", output, diagnostics) ||
        diagnostics.records()[0].id != diagnostics::server_unsupported_configuration_version.id)
        return false;

    return rejects(R"({"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true}})");
}

bool test_missing_nested_fields()
{
    return rejects(R"({"version":1,"server":{"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[]},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info"},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{}})");
}

bool test_duplicates()
{
    return rejects(R"({"version":1,"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[{"name":"a","name":"b","transport":"tcp","protocol":"json","address":"x","port":1}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","level":"debug","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true,"metrics":false},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"a","path":"b"}})");
}

bool test_unknown_properties()
{
    return rejects(R"({"version":1,"mystery":true,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true,"mystery":1},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"tcp","protocol":"json","address":"x","port":1,"enabled":true}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true,"file":{"path":"x"}},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true,"tracing":false},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true,"events":false},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json","name":"x"}})");
}

bool test_endpoints()
{
    return rejects(R"({"version":1,"server":{"endpoints":[{"name":"","transport":"tcp","protocol":"json","address":"x","port":1}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"tcp","protocol":"json","address":"x","port":1},{"name":"a","transport":"tcp","protocol":"json","address":"y","port":2}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"udp","protocol":"json","address":"x","port":1}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"tcp","protocol":"binary","address":"x","port":1}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"tcp","protocol":"json","address":"","port":1}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"tcp","protocol":"json","address":"x","port":0}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})") &&
           rejects(R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"tcp","protocol":"json","address":"x","port":65536}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})");
}

bool test_project_and_transaction()
{
    if (!rejects(R"({"version":1,"server":{"endpoints":[],"console":false},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":""}})"))
        return false;

    server_configuration output;
    output.version = 77;
    output.project.path = "unchanged.json";
    diagnostic_buffer diagnostics;
    if (load("{}", output, diagnostics)) return false;
    if (output.version != 77 || output.project.path != std::filesystem::path{"unchanged.json"}) return false;

    diagnostics.clear();
    const auto read_result = load_server_configuration_file(
        "C:/definitely-missing/server.json", operation_id{13}, diagnostics, output);
    return !read_result.ok() && !diagnostics.empty() &&
           diagnostics.records()[0].id == diagnostics::server_configuration_read_failed.id &&
           output.version == 77;
}

bool test_structured_schema_diagnostic_and_utf8_path()
{
    server_configuration output;
    diagnostic_buffer diagnostics;
    constexpr std::string_view invalid_port = R"({"version":1,"server":{"endpoints":[{"name":"a","transport":"tcp","protocol":"json","address":"x","port":0}],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"p.json"}})";
    if (load(invalid_port, output, diagnostics) || diagnostics.empty()) return false;
    const auto& record = diagnostics.records()[0];
    if (record.location.offset == 0 ||
        record.detail.find("server.endpoints[].port") == std::string::npos) return false;

    constexpr std::string_view unicode_path = R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"../\u041F\u0440\u043E\u0435\u043A\u0442/project.json"}})";
    diagnostics.clear();
    if (!load(unicode_path, output, diagnostics)) return false;
#ifdef _WIN32
    const std::filesystem::path expected{
        std::u8string{u8"C:/Server/\u041F\u0440\u043E\u0435\u043A\u0442/project.json"}};
#else
    const std::filesystem::path expected{"C:/Server/Проект/project.json"};
#endif
    return output.project.path == expected.lexically_normal();
}

} // namespace

int main()
{
    return test_valid_full_and_paths() && test_valid_variants() &&
                   test_syntax_version_and_required() && test_missing_nested_fields() &&
                   test_duplicates() && test_unknown_properties() && test_endpoints() &&
                   test_project_and_transaction() &&
                   test_structured_schema_diagnostic_and_utf8_path()
               ? 0
               : 1;
}
