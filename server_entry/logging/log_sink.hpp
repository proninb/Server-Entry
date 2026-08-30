#pragma once

#include "log_record.hpp"

namespace cw::server
{

class log_sink
{
public:
    virtual ~log_sink() = default;
    virtual void write(const log_record& record) noexcept = 0;
};

} // namespace cw::server
