#include "../server_entry/logging/console_log_sink.hpp"
#include "../server_entry/logging/logger.hpp"

#include <vector>

namespace
{

class capture_log_sink final : public cw::server::log_sink
{
public:
    void write(const cw::server::log_record& record) noexcept override
    {
        try
        {
            records.push_back(record);
        }
        catch (...)
        {
        }
    }

    std::vector<cw::server::log_record> records;
};

bool test_dispatch_and_operation()
{
    cw::server::logger log;
    capture_log_sink first;
    capture_log_sink second;
    log.add_sink(first);
    log.add_sink(second);

    log.info(cw::server::log_component::project, cw::server::operation_id{42}, "started");

    return first.records.size() == 1 && second.records.size() == 1 &&
           first.records[0].operation == cw::server::operation_id{42} &&
           first.records[0].message == "started" &&
           second.records[0].operation == first.records[0].operation;
}

bool test_no_sink_and_console() noexcept
{
    try
    {
        cw::server::logger no_sink;
        no_sink.error(cw::server::log_component::server, cw::server::operation_id{1}, "safe");

        cw::server::logger console_logger;
        cw::server::console_log_sink console;
        console_logger.add_sink(console);
        console_logger.info(cw::server::log_component::server,
                            cw::server::operation_id{2}, "console sink test");
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace

int main()
{
    return test_dispatch_and_operation() && test_no_sink_and_console() ? 0 : 1;
}
