#pragma once

#include "metrics_store.hpp"

#include <chrono>

namespace cw::server
{

template <typename Recorder>
class basic_scoped_timer
{
public:
    basic_scoped_timer(Recorder& metrics, metric_id id) noexcept
        : metrics_(metrics), id_(id), enabled_(metrics.timing_enabled(id))
    {
        if (enabled_) start_ = std::chrono::steady_clock::now();
    }

    basic_scoped_timer(const basic_scoped_timer&) = delete;
    basic_scoped_timer& operator=(const basic_scoped_timer&) = delete;

    ~basic_scoped_timer() noexcept
    {
        if (enabled_)
            metrics_.record_duration(
                id_, std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - start_));
    }

private:
    Recorder& metrics_;
    metric_id id_;
    bool enabled_ = false;
    std::chrono::steady_clock::time_point start_;
};

using scoped_timer = basic_scoped_timer<metrics_store>;

} // namespace cw::server
