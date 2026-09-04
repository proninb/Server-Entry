#pragma once

#include "log_sink.hpp"

namespace cw::server {

// Writes fully constructed log records to the process console.
// console_log_sink formats each record as one UTC text line and performs
// best-effort output without propagating console failures to logger.
class console_log_sink final : public log_sink {
public:
    void write(const log_record& record) noexcept override;
};

} // namespace cw::server
