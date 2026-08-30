#pragma once

#include "metrics_store.hpp"

#include <chrono>

namespace cw::server
{

class scoped_timer
{
public:
    scoped_timer(metrics_store& metrics, metric_id id) noexcept
        : metrics_(metrics), id_(id), start_(std::chrono::steady_clock::now())
    {
    }

    scoped_timer(const scoped_timer&) = delete;
    scoped_timer& operator=(const scoped_timer&) = delete;

    ~scoped_timer() noexcept
    {
        metrics_.record_duration(
            id_, std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start_));
    }

private:
    metrics_store& metrics_;
    metric_id id_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace cw::server
