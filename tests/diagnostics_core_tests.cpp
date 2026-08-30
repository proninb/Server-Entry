#include "../server_entry/diagnostics/diagnostic_buffer.hpp"
#include "../server_entry/diagnostics/diagnostic_registry.hpp"
#include "../server_entry/server_context.hpp"

#include <string_view>

namespace
{

bool test_operation_ids()
{
    cw::server::server_context server;
    const auto first = server.next_operation_id();
    const auto second = server.next_operation_id();
    return first && second.value() == first.value() + 1;
}

bool test_registry()
{
    const auto* descriptor = cw::server::diagnostic_registry.find(
        cw::server::diagnostics::runtime_attach_failed.id);
    return descriptor != nullptr && descriptor->domain == cw::server::diagnostic_domain::runtime &&
           descriptor->name == "runtime.attach_failed";
}

bool test_buffer()
{
    using namespace cw::server;

    diagnostic_buffer buffer;
    buffer.reserve(3);
    buffer.emit({diagnostic_id{9}, diagnostic_severity::warning, operation_id{1},
                 {source_id{2}, 4, 1}, "z"});
    buffer.emit({diagnostic_id{3}, diagnostic_severity::error, operation_id{1},
                 {source_id{1}, 8, 1}, "b"});
    buffer.emit({diagnostic_id{2}, diagnostic_severity::note, operation_id{1},
                 {source_id{1}, 8, 1}, "a"});

    if (buffer.empty() || !buffer.has_errors() || buffer.records().size() != 3)
    {
        return false;
    }

    buffer.sort_deterministic();
    const auto records = buffer.records();
    if (records[0].id != diagnostic_id{2} || records[1].id != diagnostic_id{3} ||
        records[2].location.source != source_id{2})
    {
        return false;
    }

    buffer.clear();
    if (!buffer.empty() || buffer.has_errors())
    {
        return false;
    }

    buffer.emit({diagnostic_id{4}, diagnostic_severity::note, operation_id{2}, {}, "reused"});
    return buffer.records().size() == 1 && buffer.records()[0].detail == "reused";
}

} // namespace

int main()
{
    return test_operation_ids() && test_registry() && test_buffer() ? 0 : 1;
}
