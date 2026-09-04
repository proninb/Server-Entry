#include "project_configuration_loader.hpp"

#include "../diagnostics/diagnostic_descriptor.hpp"
#include "../json/json_parser.hpp"
#include "../metrics/scoped_timer.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace cw::server {
namespace {

enum class context : std::uint8_t {
    root,
    items,
    item,
    configuration,
    abi,
    invalid
};

enum class field : std::uint8_t {
    none,
    version,
    name,
    project,
    configuration,
    path,
    role,
    abi,
    target,
    pack,
    unknown
};

enum class schema_failure : std::uint8_t {
    none,
    wrong_root_type,
    unknown_property,
    duplicate_property,
    wrong_field_type,
    missing_required_field,
    invalid_name,
    invalid_path,
    invalid_role,
    invalid_target,
    invalid_pack,
    nesting_too_deep
};

struct schema_error {
    schema_failure code = schema_failure::none;
    context owner = context::invalid;
    field member = field::none;
    std::size_t offset = 0;
};

constexpr std::string_view failure_detail(const schema_error& error) noexcept {
    switch (error.code) {
    case schema_failure::wrong_root_type:
        return "project configuration root must be an object";

    case schema_failure::unknown_property:
        return "project configuration contains an unknown property";

    case schema_failure::duplicate_property:
        return "project configuration contains a duplicate property";

    case schema_failure::nesting_too_deep:
        return "project configuration nesting is too deep";

    default:
        break;
    }

    if (error.owner == context::root && error.member == field::name) {
        return "name: expected a non-empty string";
    }

    if (error.owner == context::item && error.member == field::path) {
        return "project[].path: expected a non-empty UTF-8 path string";
    }

    if (error.owner == context::item && error.member == field::role) {
        return "project[].role: expected type, source, or project";
    }

    if (error.owner == context::abi && error.member == field::target) {
        return "configuration.abi.target: expected windows-x64 or posix-x64";
    }

    if (error.owner == context::abi && error.member == field::pack) {
        return "configuration.abi.pack: expected 1, 2, 4, 8, or 16";
    }

    switch (error.code) {
    case schema_failure::wrong_field_type:
        return "project configuration field has the wrong JSON type";

    case schema_failure::missing_required_field:
        return "project configuration is missing a required field";

    case schema_failure::invalid_name:
        return "project name must not be empty";

    case schema_failure::invalid_path:
        return "project item path must not be empty";

    case schema_failure::invalid_role:
        return "project item role is unsupported";

    case schema_failure::invalid_target:
        return "project ABI target is unsupported";

    case schema_failure::invalid_pack:
        return "project ABI pack is unsupported";

    default:
        return {};
    }
}

// Converts the UTF-8 path stored in project.json to the platform filesystem path.
std::filesystem::path path_from_utf8(std::string_view value) {
#ifdef _WIN32
    std::u8string bytes(value.size(), u8'\0');

    if (!value.empty()) {
        std::memcpy(bytes.data(), value.data(), value.size());
    }

    return std::filesystem::path{bytes};
#else
    return std::filesystem::path{value};
#endif
}

// Streaming schema handler for project_configuration.
// It validates structure and supported values while materializing the typed
// configuration directly, without building an intermediate JSON DOM.
class project_configuration_handler final : public json_event_handler {
public:
    void location(std::size_t offset) noexcept override {
        current_offset = offset;
    }

    void object_begin() noexcept override {
        if (stopped()) {
            return;
        }

        if (depth == 0) {
            push(context::root);
            root_object = true;
            return;
        }

        const auto next = object_context();

        if (next == context::invalid) {
            fail(schema_failure::wrong_field_type, pending);
            return;
        }

        consume_pending();
        push(next);

        if (stopped()) {
            return;
        }

        if (next == context::item) {
            item = {};
            item_seen = 0;
        }
    }

    void object_end() noexcept override {
        if (stopped()) {
            return;
        }

        if (depth == 0) {
            fail(schema_failure::wrong_field_type);
            return;
        }

        const auto ending = current();
        validate_required(ending);

        if (stopped()) {
            return;
        }

        if (ending == context::item) {
            try {
                candidate.project.push_back(std::move(item));
            }
            catch (const std::bad_alloc&) {
                internal_failure_flag = true;
            }
            catch (const std::length_error&) {
                internal_failure_flag = true;
            }

            if (stopped()) {
                return;
            }
        }

        pop();
    }

    void array_begin() noexcept override {
        if (stopped()) {
            return;
        }

        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
            return;
        }

        const auto next =
            current() == context::root && pending == field::project
                ? context::items
                : context::invalid;

        if (next == context::invalid) {
            fail(schema_failure::wrong_field_type, pending);
            return;
        }

        consume_pending();
        push(next);
    }

    void array_end() noexcept override {
        if (stopped()) {
            return;
        }

        if (depth == 0 || current() != context::items) {
            fail(schema_failure::wrong_field_type);
            return;
        }

        pop();
    }

    void key(std::string_view value) noexcept override {
        if (stopped()) {
            return;
        }

        if (depth == 0) {
            fail(schema_failure::wrong_field_type);
            return;
        }

        pending = identify(current(), value);

        if (pending == field::unknown) {
            fail(schema_failure::unknown_property);
            return;
        }

        const auto bit = field_bit(pending);
        auto& seen = seen_mask(current());

        if ((seen & bit) != 0) {
            fail(schema_failure::duplicate_property);
            return;
        }

        seen |= bit;
    }

    void string(std::string_view value) noexcept override {
        if (stopped()) {
            return;
        }

        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
            return;
        }

        try {
            if (current() == context::root && pending == field::name) {
                if (value.empty()) {
                    fail(schema_failure::invalid_name, field::name);
                    return;
                }

                candidate.name.assign(value);
            }
            else if (current() == context::item && pending == field::path) {
                if (value.empty()) {
                    fail(schema_failure::invalid_path, field::path);
                    return;
                }

                item.path = path_from_utf8(value);
            }
            else if (current() == context::item && pending == field::role) {
                if (value == "type") {
                    item.role = project_item_role::type;
                }
                else if (value == "source") {
                    item.role = project_item_role::source;
                }
                else if (value == "project") {
                    item.role = project_item_role::project;
                }
                else {
                    fail(schema_failure::invalid_role, field::role);
                    return;
                }
            }
            else if (current() == context::abi && pending == field::target) {
                if (value == "windows-x64") {
                    candidate.abi.target = abi_target::windows_x64;
                }
                else if (value == "posix-x64") {
                    candidate.abi.target = abi_target::posix_x64;
                }
                else {
                    fail(schema_failure::invalid_target, field::target);
                    return;
                }
            }
            else {
                fail(schema_failure::wrong_field_type);
                return;
            }
        }
        catch (const std::bad_alloc&) {
            internal_failure_flag = true;
        }
        catch (const std::length_error&) {
            internal_failure_flag = true;
        }
        catch (const std::filesystem::filesystem_error&) {
            fail(schema_failure::invalid_path);
        }

        if (!stopped()) {
            consume_pending();
        }
    }

    void integer(std::int64_t value) noexcept override {
        if (stopped()) {
            return;
        }

        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
            return;
        }

        if (current() == context::root && pending == field::version) {
            if (value < 0 ||
                value > std::numeric_limits<std::uint32_t>::max()) {
                fail(schema_failure::wrong_field_type);
            }
            else {
                candidate.version = static_cast<std::uint32_t>(value);
            }
        }
        else if (current() == context::abi && pending == field::pack) {
            if (value != 1 &&
                value != 2 &&
                value != 4 &&
                value != 8 &&
                value != 16) {
                fail(schema_failure::invalid_pack, field::pack);
            }
            else {
                candidate.abi.pack = static_cast<std::uint32_t>(value);
            }
        }
        else {
            fail(schema_failure::wrong_field_type);
        }

        if (!stopped()) {
            consume_pending();
        }
    }

    void boolean(bool) noexcept override {
        reject_scalar();
    }

    void number(double) noexcept override {
        reject_scalar();
    }

    void null() noexcept override {
        reject_scalar();
    }

    [[nodiscard]] bool valid() const noexcept {
        return root_object && !stopped() && depth == 0;
    }

    [[nodiscard]] bool internal_failure() const noexcept {
        return internal_failure_flag;
    }

    [[nodiscard]] const schema_error& error() const noexcept {
        return failure;
    }

    [[nodiscard]] project_configuration take() {
        return std::move(candidate);
    }

private:
    [[nodiscard]] context object_context() const noexcept {
        if (current() == context::root && pending == field::configuration) {
            return context::configuration;
        }

        if (current() == context::configuration && pending == field::abi) {
            return context::abi;
        }

        if (current() == context::items) {
            return context::item;
        }

        return context::invalid;
    }

    static field identify(context owner, std::string_view value) noexcept {
        if (owner == context::root) {
            if (value == "version") return field::version;
            if (value == "name") return field::name;
            if (value == "project") return field::project;
            if (value == "configuration") return field::configuration;
        }
        else if (owner == context::item) {
            if (value == "path") return field::path;
            if (value == "role") return field::role;
        }
        else if (owner == context::configuration && value == "abi") {
            return field::abi;
        }
        else if (owner == context::abi) {
            if (value == "target") return field::target;
            if (value == "pack") return field::pack;
        }

        return field::unknown;
    }

    static constexpr std::uint32_t field_bit(field value) noexcept {
        return std::uint32_t{1} << static_cast<std::uint8_t>(value);
    }

    std::uint32_t& seen_mask(context owner) noexcept {
        switch (owner) {
        case context::root:
            return root_seen;

        case context::item:
            return item_seen;

        case context::configuration:
            return configuration_seen;

        case context::abi:
            return abi_seen;

        default:
            return invalid_seen;
        }
    }

    void validate_required(context owner) noexcept {
        switch (owner) {
        case context::root:
            require(owner, root_seen, field::version);
            require(owner, root_seen, field::name);
            require(owner, root_seen, field::project);
            require(owner, root_seen, field::configuration);
            break;

        case context::item:
            require(owner, item_seen, field::path);
            require(owner, item_seen, field::role);
            break;

        case context::configuration:
            require(owner, configuration_seen, field::abi);
            break;

        case context::abi:
            require(owner, abi_seen, field::target);
            require(owner, abi_seen, field::pack);
            break;

        default:
            break;
        }
    }

    void require(
        context owner,
        std::uint32_t actual,
        field member) noexcept {

        if (stopped()) {
            return;
        }

        if ((actual & field_bit(member)) == 0) {
            fail(schema_failure::missing_required_field, member, owner);
        }
    }

    void reject_scalar() noexcept {
        if (stopped()) {
            return;
        }

        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
        }
        else {
            fail(schema_failure::wrong_field_type);
        }
    }

    [[nodiscard]] bool stopped() const noexcept {
        return failure.code != schema_failure::none ||
               internal_failure_flag;
    }

    void consume_pending() noexcept {
        pending = field::none;
    }

    [[nodiscard]] context current() const noexcept {
        return depth == 0
                   ? context::invalid
                   : stack[depth - 1];
    }

    void push(context value) noexcept {
        if (depth == stack.size()) {
            fail(schema_failure::nesting_too_deep);
            return;
        }

        stack[depth++] = value;
    }

    void pop() noexcept {
        if (depth > 0) {
            --depth;
        }
    }

    void fail(
        schema_failure code,
        field member = field::none,
        context owner = context::invalid) noexcept {

        if (stopped()) {
            return;
        }

        failure = {
            code,
            owner == context::invalid ? current() : owner,
            member == field::none ? pending : member,
            current_offset
        };
    }

    project_configuration candidate;
    project_item_configuration item;
    std::array<context, 16> stack{};
    std::size_t depth = 0;
    field pending = field::none;
    schema_error failure;
    std::size_t current_offset = 0;
    bool internal_failure_flag = false;
    bool root_object = false;
    std::uint32_t root_seen = 0;
    std::uint32_t item_seen = 0;
    std::uint32_t configuration_seen = 0;
    std::uint32_t abi_seen = 0;
    std::uint32_t invalid_seen = 0;
};

// Emits diagnostics on a best-effort basis so diagnostic allocation failure
// cannot replace the original configuration failure.
bool try_emit(
    diagnostic_buffer& diagnostics,
    const diagnostic_descriptor& descriptor,
    operation_id operation,
    std::string_view detail = {},
    std::size_t offset = 0) noexcept {

    try {
        diagnostics.emit({
            descriptor.id,
            descriptor.default_severity,
            operation,
            {{}, static_cast<std::uint32_t>(offset), 0},
            std::string{detail}
        });

        return true;
    }
    catch (const std::bad_alloc&) {
        return false;
    }
    catch (const std::length_error&) {
        return false;
    }
}

} // namespace

// Performs the shared transactional load path used by both public entry points.
// output is replaced only after parsing, schema/version validation, and path
// normalization have all succeeded.
static status load_project_configuration_impl(
    std::string_view text,
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    project_configuration& output) noexcept {

    try {
        project_configuration_handler handler;
        json_error json_failure;
        json_parser parser{text};

        bool parsed = false;

        {
            scoped_timer parse_timer{
                metrics,
                metric_id::project_configuration_parse_duration
            };

            parsed = parser.parse(handler, json_failure);
        }

        if (!parsed) {
            if (json_failure.code == json_error_code::allocation_failed) {
                try_emit(
                    diagnostics,
                    diagnostics::project_initialization_failed,
                    operation);

                return {status_code::initialization_failed};
            }

            try_emit(
                diagnostics,
                diagnostics::project_invalid_json,
                operation,
                "project.json contains invalid JSON",
                json_failure.offset);

            return {status_code::configuration_failed};
        }

        if (handler.internal_failure()) {
            try_emit(
                diagnostics,
                diagnostics::project_initialization_failed,
                operation);

            return {status_code::initialization_failed};
        }

        if (!handler.valid()) {
            try_emit(
                diagnostics,
                diagnostics::project_invalid_configuration,
                operation,
                failure_detail(handler.error()),
                handler.error().offset);

            return {status_code::configuration_failed};
        }

        auto candidate = handler.take();

        if (candidate.version != current_project_configuration_version) {
            try_emit(
                diagnostics,
                diagnostics::project_unsupported_configuration_version,
                operation);

            return {status_code::configuration_failed};
        }

        {
            scoped_timer path_timer{
                metrics,
                metric_id::project_configuration_path_resolution_duration
            };

            std::error_code error;
            auto absolute_configuration =
                std::filesystem::absolute(configuration_path, error);

            if (error) {
                try_emit(
                    diagnostics,
                    diagnostics::project_invalid_configuration,
                    operation,
                    "project configuration path could not be resolved");

                return {status_code::configuration_failed};
            }

            const auto directory =
                absolute_configuration.lexically_normal().parent_path();

            for (auto& item : candidate.project) {
                item.path =
                    item.path.is_absolute()
                        ? item.path.lexically_normal()
                        : (directory / item.path).lexically_normal();
            }
        }

        metrics.increment(
            metric_id::project_configuration_item_count,
            static_cast<std::uint64_t>(candidate.project.size()));

        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&) {
        try_emit(
            diagnostics,
            diagnostics::project_initialization_failed,
            operation);

        return {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        try_emit(
            diagnostics,
            diagnostics::project_initialization_failed,
            operation);

        return {status_code::initialization_failed};
    }
    catch (const std::filesystem::filesystem_error&) {
        try_emit(
            diagnostics,
            diagnostics::project_invalid_configuration,
            operation,
            "project item path could not be resolved");

        return {status_code::configuration_failed};
    }
}

status load_project_configuration(
    std::string_view text,
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    project_configuration& output) noexcept {

    metrics.increment(metric_id::project_configuration_load_count);

    scoped_timer total_timer{
        metrics,
        metric_id::project_configuration_load_duration
    };

    return load_project_configuration_impl(
        text,
        configuration_path,
        operation,
        diagnostics,
        metrics,
        output);
}

status load_project_configuration_file(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    metrics_store& metrics,
    project_configuration& output) noexcept {

    metrics.increment(metric_id::project_configuration_load_count);

    scoped_timer total_timer{
        metrics,
        metric_id::project_configuration_load_duration
    };

    try {
        std::ifstream input{
            configuration_path,
            std::ios::binary
        };

        if (!input) {
            try_emit(
                diagnostics,
                diagnostics::project_configuration_read_failed,
                operation,
                "project configuration file could not be opened");

            return {status_code::configuration_failed};
        }

        std::string text{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };

        if (input.bad()) {
            try_emit(
                diagnostics,
                diagnostics::project_configuration_read_failed,
                operation,
                "project configuration file could not be read");

            return {status_code::configuration_failed};
        }

        return load_project_configuration_impl(
            text,
            configuration_path,
            operation,
            diagnostics,
            metrics,
            output);
    }
    catch (const std::bad_alloc&) {
        try_emit(
            diagnostics,
            diagnostics::project_initialization_failed,
            operation);

        return {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        try_emit(
            diagnostics,
            diagnostics::project_initialization_failed,
            operation);

        return {status_code::initialization_failed};
    }
}

} // namespace cw::server
