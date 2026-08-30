#include "server_configuration_loader.hpp"

#include "../diagnostics/diagnostic_descriptor.hpp"
#include "../json/json_parser.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <iterator>
#include <new>
#include <system_error>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace cw::server
{
namespace
{

enum class context : std::uint8_t
{
    root,
    communication,
    endpoints,
    endpoint,
    logging,
    telemetry,
    project,
    invalid,
};

enum class field : std::uint8_t
{
    none,
    version,
    server,
    logging,
    telemetry,
    project,
    endpoints,
    console,
    name,
    transport,
    protocol,
    address,
    port,
    level,
    metrics,
    path,
    unknown,
};

enum class schema_failure : std::uint8_t
{
    none,
    wrong_root_type,
    unknown_property,
    duplicate_property,
    wrong_field_type,
    missing_required_field,
    invalid_endpoint,
    duplicate_endpoint_name,
    invalid_log_level,
    invalid_project_path,
    nesting_too_deep,
};

struct schema_error
{
    schema_failure code = schema_failure::none;
    context owner = context::invalid;
    field member = field::none;
    std::size_t offset = 0;
};

constexpr std::string_view failure_detail(const schema_error& error) noexcept
{
    switch (error.code)
    {
    case schema_failure::wrong_root_type: return "server configuration root must be an object";
    case schema_failure::unknown_property: return "configuration contains an unknown property";
    case schema_failure::duplicate_property: return "configuration contains a duplicate property";
    case schema_failure::duplicate_endpoint_name: return "endpoint names must be unique";
    case schema_failure::nesting_too_deep: return "configuration nesting is too deep";
    default: break;
    }

    if (error.owner == context::endpoint)
    {
        switch (error.member)
        {
        case field::name: return "server.endpoints[].name: expected a non-empty unique string";
        case field::transport: return "server.endpoints[].transport: expected \"tcp\"";
        case field::protocol: return "server.endpoints[].protocol: expected \"json\"";
        case field::address: return "server.endpoints[].address: expected a non-empty string";
        case field::port: return "server.endpoints[].port: expected integer 1..65535";
        default: break;
        }
    }
    if (error.owner == context::logging && error.member == field::level)
        return "logging.level: expected trace, debug, info, warning, error, or critical";
    if (error.owner == context::project && error.member == field::path)
        return "project.path: expected a non-empty UTF-8 path string";

    switch (error.code)
    {
    case schema_failure::wrong_field_type: return "configuration field has the wrong JSON type";
    case schema_failure::missing_required_field: return "configuration is missing a required field";
    case schema_failure::invalid_endpoint: return "endpoint configuration is invalid";
    case schema_failure::invalid_log_level: return "logging.level is not supported";
    case schema_failure::invalid_project_path: return "project.path must not be empty";
    case schema_failure::wrong_root_type:
    case schema_failure::unknown_property:
    case schema_failure::duplicate_property:
    case schema_failure::duplicate_endpoint_name:
    case schema_failure::nesting_too_deep:
    case schema_failure::none: return {};
    }
    return {};
}

class server_configuration_handler final : public json_event_handler
{
public:
    void location(std::size_t offset) noexcept override { current_offset_ = offset; }

    void object_begin() noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0)
        {
            push(context::root);
            root_object_ = true;
            return;
        }

        const auto next = object_context();
        if (next == context::invalid)
        {
            fail(schema_failure::wrong_field_type, pending_);
            return;
        }
        consume_pending();
        push(next);
        if (stopped()) return;
        if (next == context::endpoint)
        {
            endpoint_ = {};
            endpoint_seen_ = 0;
            endpoint_name_offset_ = 0;
        }
    }

    void object_end() noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0) { fail(schema_failure::wrong_field_type); return; }
        const auto ending = current();
        validate_required(ending);
        if (stopped()) return;
        if (ending == context::endpoint) finish_endpoint();
        if (stopped()) return;
        pop();
    }

    void array_begin() noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0) { fail(schema_failure::wrong_root_type); return; }
        context next = context::invalid;
        if (depth_ > 0 && current() == context::communication && pending_ == field::endpoints)
            next = context::endpoints;
        if (next == context::invalid)
        {
            fail(schema_failure::wrong_field_type, pending_);
            return;
        }
        consume_pending();
        push(next);
        if (stopped()) return;
    }

    void array_end() noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0 || current() != context::endpoints)
        {
            fail(schema_failure::wrong_field_type);
            return;
        }
        pop();
    }

    void key(std::string_view value) noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0) { fail(schema_failure::wrong_field_type); return; }
        pending_ = identify(current(), value);
        if (pending_ == field::unknown) { fail(schema_failure::unknown_property); return; }

        const auto bit = field_bit(pending_);
        auto& seen = seen_mask(current());
        if ((seen & bit) != 0) { fail(schema_failure::duplicate_property); return; }
        seen |= bit;
    }

    void string(std::string_view value) noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0) { fail(schema_failure::wrong_root_type); return; }
        try
        {
            if (current() == context::endpoint)
            {
                if (pending_ == field::name)
                {
                    endpoint_name_offset_ = current_offset_;
                    if (value.empty()) fail(schema_failure::invalid_endpoint, field::name);
                    else endpoint_.name.assign(value);
                }
                else if (pending_ == field::address)
                {
                    if (value.empty()) fail(schema_failure::invalid_endpoint, field::address);
                    else endpoint_.address.assign(value);
                }
                else if (pending_ == field::transport)
                {
                    if (value != "tcp") { fail(schema_failure::invalid_endpoint); return; }
                    endpoint_.transport = transport_kind::tcp;
                }
                else if (pending_ == field::protocol)
                {
                    if (value != "json") { fail(schema_failure::invalid_endpoint); return; }
                    endpoint_.protocol = protocol_kind::json;
                }
                else { fail(schema_failure::wrong_field_type); return; }
            }
            else if (current() == context::logging && pending_ == field::level)
            {
                if (!parse_level(value, candidate_.logging.minimum_level))
                {
                    fail(schema_failure::invalid_log_level);
                    return;
                }
            }
            else if (current() == context::project && pending_ == field::path)
            {
                if (value.empty()) fail(schema_failure::invalid_project_path, field::path);
                else project_path_.assign(value);
            }
            else { fail(schema_failure::wrong_field_type); return; }
        }
        catch (const std::bad_alloc&) { internal_failure_ = true; }
        catch (const std::length_error&) { internal_failure_ = true; }
        if (!stopped()) consume_pending();
    }

    void integer(std::int64_t value) noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0) { fail(schema_failure::wrong_root_type); return; }
        if (current() == context::root && pending_ == field::version)
        {
            if (value < 0 || value > std::numeric_limits<std::uint32_t>::max())
                fail(schema_failure::wrong_field_type);
            else candidate_.version = static_cast<std::uint32_t>(value);
        }
        else if (current() == context::endpoint && pending_ == field::port)
        {
            if (value < 1 || value > 65535) fail(schema_failure::invalid_endpoint);
            else endpoint_.port = static_cast<std::uint16_t>(value);
        }
        else fail(schema_failure::wrong_field_type);
        if (!stopped()) consume_pending();
    }

    void boolean(bool value) noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0) { fail(schema_failure::wrong_root_type); return; }
        if (current() == context::communication && pending_ == field::console)
            candidate_.communication.console = value;
        else if (current() == context::logging && pending_ == field::console)
            candidate_.logging.console = value;
        else if (current() == context::telemetry && pending_ == field::metrics)
            candidate_.telemetry.metrics = value;
        else fail(schema_failure::wrong_field_type);
        if (!stopped()) consume_pending();
    }

    void number(double) noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0) { fail(schema_failure::wrong_root_type); return; }
        reject_scalar();
    }
    void null() noexcept override
    {
        if (stopped()) return;
        if (depth_ == 0) { fail(schema_failure::wrong_root_type); return; }
        reject_scalar();
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return root_object_ && error_.code == schema_failure::none && !internal_failure_ && depth_ == 0;
    }

    [[nodiscard]] const schema_error& error() const noexcept { return error_; }
    [[nodiscard]] bool internal_failure() const noexcept { return internal_failure_; }
    [[nodiscard]] server_configuration take() { return std::move(candidate_); }
    [[nodiscard]] const std::string& project_path() const noexcept { return project_path_; }

private:
    static bool parse_level(std::string_view value, log_level& output) noexcept
    {
        if (value == "trace") output = log_level::trace;
        else if (value == "debug") output = log_level::debug;
        else if (value == "info") output = log_level::info;
        else if (value == "warning") output = log_level::warning;
        else if (value == "error") output = log_level::error;
        else if (value == "critical") output = log_level::critical;
        else return false;
        return true;
    }

    context object_context() const noexcept
    {
        if (current() == context::root)
        {
            if (pending_ == field::server) return context::communication;
            if (pending_ == field::logging) return context::logging;
            if (pending_ == field::telemetry) return context::telemetry;
            if (pending_ == field::project) return context::project;
        }
        if (current() == context::endpoints) return context::endpoint;
        return context::invalid;
    }

    static field identify(context owner, std::string_view value) noexcept
    {
        if (owner == context::root)
        {
            if (value == "version") return field::version;
            if (value == "server") return field::server;
            if (value == "logging") return field::logging;
            if (value == "telemetry") return field::telemetry;
            if (value == "project") return field::project;
        }
        else if (owner == context::communication)
        {
            if (value == "endpoints") return field::endpoints;
            if (value == "console") return field::console;
        }
        else if (owner == context::endpoint)
        {
            if (value == "name") return field::name;
            if (value == "transport") return field::transport;
            if (value == "protocol") return field::protocol;
            if (value == "address") return field::address;
            if (value == "port") return field::port;
        }
        else if (owner == context::logging)
        {
            if (value == "level") return field::level;
            if (value == "console") return field::console;
        }
        else if (owner == context::telemetry && value == "metrics") return field::metrics;
        else if (owner == context::project && value == "path") return field::path;
        return field::unknown;
    }

    static constexpr std::uint32_t field_bit(field value) noexcept
    {
        return std::uint32_t{1} << static_cast<std::uint8_t>(value);
    }

    std::uint32_t& seen_mask(context owner) noexcept
    {
        switch (owner)
        {
        case context::root: return root_seen_;
        case context::communication: return communication_seen_;
        case context::endpoint: return endpoint_seen_;
        case context::logging: return logging_seen_;
        case context::telemetry: return telemetry_seen_;
        case context::project: return project_seen_;
        default: return invalid_seen_;
        }
    }

    void validate_required(context owner) noexcept
    {
        std::uint32_t actual = 0;
        switch (owner)
        {
        case context::root:
            actual = root_seen_;
            require(owner, actual, field::version); require(owner, actual, field::server);
            require(owner, actual, field::logging); require(owner, actual, field::telemetry);
            require(owner, actual, field::project);
            break;
        case context::communication:
            actual = communication_seen_;
            require(owner, actual, field::endpoints); require(owner, actual, field::console);
            break;
        case context::endpoint:
            actual = endpoint_seen_;
            require(owner, actual, field::name); require(owner, actual, field::transport);
            require(owner, actual, field::protocol); require(owner, actual, field::address);
            require(owner, actual, field::port);
            break;
        case context::logging:
            actual = logging_seen_;
            require(owner, actual, field::level); require(owner, actual, field::console);
            break;
        case context::telemetry:
            actual = telemetry_seen_;
            require(owner, actual, field::metrics);
            break;
        case context::project:
            actual = project_seen_;
            require(owner, actual, field::path);
            break;
        default: return;
        }
    }

    void require(context owner, std::uint32_t actual, field member) noexcept
    {
        if ((actual & field_bit(member)) == 0)
            fail(schema_failure::missing_required_field, member, owner);
    }

    void finish_endpoint() noexcept
    {
        for (const auto& existing : candidate_.communication.endpoints)
            if (existing.name == endpoint_.name)
            {
                fail(schema_failure::duplicate_endpoint_name, field::name,
                     context::endpoint, endpoint_name_offset_);
                return;
            }
        try { candidate_.communication.endpoints.push_back(std::move(endpoint_)); }
        catch (const std::bad_alloc&) { internal_failure_ = true; }
        catch (const std::length_error&) { internal_failure_ = true; }
    }

    void reject_scalar() noexcept { fail(schema_failure::wrong_field_type); }
    void consume_pending() noexcept { pending_ = field::none; }
    context current() const noexcept { return depth_ == 0 ? context::invalid : stack_[depth_ - 1]; }
    [[nodiscard]] bool stopped() const noexcept
    {
        return error_.code != schema_failure::none || internal_failure_;
    }

    void push(context value) noexcept
    {
        if (depth_ == stack_.size()) { fail(schema_failure::nesting_too_deep); return; }
        stack_[depth_++] = value;
    }
    void pop() noexcept { if (depth_ > 0) --depth_; }
    void fail(schema_failure value, field member = field::none,
              context owner = context::invalid,
              std::size_t offset = std::numeric_limits<std::size_t>::max()) noexcept
    {
        if (error_.code != schema_failure::none) return;
        error_ = {value,
                  owner == context::invalid ? current() : owner,
                  member == field::none ? pending_ : member,
                  offset == std::numeric_limits<std::size_t>::max() ? current_offset_ : offset};
    }

    server_configuration candidate_;
    endpoint_configuration endpoint_;
    std::string project_path_;
    std::array<context, 16> stack_{};
    std::size_t depth_ = 0;
    field pending_ = field::none;
    schema_error error_;
    std::size_t current_offset_ = 0;
    std::size_t endpoint_name_offset_ = 0;
    bool internal_failure_ = false;
    bool root_object_ = false;
    std::uint32_t root_seen_ = 0;
    std::uint32_t communication_seen_ = 0;
    std::uint32_t endpoint_seen_ = 0;
    std::uint32_t logging_seen_ = 0;
    std::uint32_t telemetry_seen_ = 0;
    std::uint32_t project_seen_ = 0;
    std::uint32_t invalid_seen_ = 0;
};

bool try_emit(diagnostic_buffer& diagnostics, const diagnostic_descriptor& descriptor,
              operation_id operation, std::string_view detail = {},
              std::size_t offset = 0) noexcept
{
    try
    {
        diagnostics.emit({descriptor.id, descriptor.default_severity, operation,
                          {{}, static_cast<std::uint32_t>(offset), 0}, std::string{detail}});
        return true;
    }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
}

std::filesystem::path filesystem_path_from_utf8(std::string_view value)
{
#ifdef _WIN32
    std::u8string utf8(value.size(), u8'\0');
    if (!value.empty())
    {
        std::memcpy(utf8.data(), value.data(), value.size());
    }
    return std::filesystem::path{utf8};
#else
    return std::filesystem::path{value};
#endif
}

} // namespace

status load_server_configuration(std::string_view text,
                                 const std::filesystem::path& configuration_path,
                                 operation_id operation,
                                 diagnostic_buffer& diagnostics,
                                 server_configuration& output) noexcept
{
    try
    {
        server_configuration_handler handler;
        json_error json_failure;
        json_parser parser{text};
        if (!parser.parse(handler, json_failure))
        {
            if (json_failure.code == json_error_code::allocation_failed)
            {
                try_emit(diagnostics, diagnostics::server_initialization_failed, operation);
                return {status_code::initialization_failed};
            }
            try_emit(diagnostics, diagnostics::server_invalid_json, operation,
                     "server.json contains invalid JSON", json_failure.offset);
            return {status_code::configuration_failed};
        }
        if (handler.internal_failure())
        {
            try_emit(diagnostics, diagnostics::server_initialization_failed, operation);
            return {status_code::initialization_failed};
        }
        if (!handler.valid())
        {
            try_emit(diagnostics, diagnostics::server_invalid_configuration, operation,
                     failure_detail(handler.error()), handler.error().offset);
            return {status_code::configuration_failed};
        }

        auto candidate = handler.take();
        if (candidate.version != current_server_configuration_version)
        {
            try_emit(diagnostics, diagnostics::server_unsupported_configuration_version,
                     operation);
            return {status_code::configuration_failed};
        }

        std::error_code filesystem_error;
        auto absolute_configuration =
            std::filesystem::absolute(configuration_path, filesystem_error);
        if (filesystem_error)
        {
            try_emit(diagnostics, diagnostics::server_configuration_read_failed, operation,
                     "server configuration path could not be resolved");
            return {status_code::configuration_failed};
        }
        absolute_configuration = absolute_configuration.lexically_normal();

        const auto configured_project = filesystem_path_from_utf8(handler.project_path());
        candidate.project.path = configured_project.is_absolute()
                                     ? configured_project.lexically_normal()
                                     : (absolute_configuration.parent_path() / configured_project).lexically_normal();

        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        try_emit(diagnostics, diagnostics::server_initialization_failed, operation);
        return {status_code::initialization_failed};
    }
    catch (const std::length_error&)
    {
        try_emit(diagnostics, diagnostics::server_initialization_failed, operation);
        return {status_code::initialization_failed};
    }
    catch (const std::filesystem::filesystem_error&)
    {
        const schema_error error{schema_failure::invalid_project_path,
                                 context::project, field::path, 0};
        try_emit(diagnostics, diagnostics::server_invalid_configuration, operation,
                 failure_detail(error));
        return {status_code::configuration_failed};
    }
}

status load_server_configuration_file(const std::filesystem::path& configuration_path,
                                      operation_id operation,
                                      diagnostic_buffer& diagnostics,
                                      server_configuration& output) noexcept
{
    try
    {
        std::ifstream input{configuration_path, std::ios::binary};
        if (!input)
        {
            try_emit(diagnostics, diagnostics::server_configuration_read_failed, operation,
                     "server configuration file could not be opened");
            return {status_code::configuration_failed};
        }

        std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if (input.bad())
        {
            try_emit(diagnostics, diagnostics::server_configuration_read_failed, operation,
                     "server configuration file could not be read");
            return {status_code::configuration_failed};
        }
        return load_server_configuration(text, configuration_path, operation, diagnostics, output);
    }
    catch (const std::bad_alloc&)
    {
        try_emit(diagnostics, diagnostics::server_initialization_failed, operation);
        return {status_code::initialization_failed};
    }
    catch (const std::length_error&)
    {
        try_emit(diagnostics, diagnostics::server_initialization_failed, operation);
        return {status_code::initialization_failed};
    }
}

} // namespace cw::server
