#pragma once

#include "log_record.hpp"

namespace cw::server {

// Defines the output boundary for one fully constructed log record.
// log_sink implementations receive records from logger and own their destination
// behavior such as console, file, or external transport output.
class log_sink {
public:
    virtual ~log_sink() = default;
    virtual void write(const log_record& record) noexcept = 0;
};

} // namespace cw::server
