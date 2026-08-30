#pragma once

#include "log_sink.hpp"

namespace cw::server
{

class console_log_sink final : public log_sink
{
public:
    void write(const log_record& record) noexcept override;
};

} // namespace cw::server
