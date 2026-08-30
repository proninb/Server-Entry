#pragma once

#include "metric.hpp"

#include <array>
#include <cstdint>

namespace cw::server
{

struct counter_snapshot
{
    std::uint64_t value = 0;
};

struct gauge_snapshot
{
    std::int64_t value = 0;
};

struct duration_snapshot
{
    std::uint64_t count = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t max_ns = 0;
};

struct metrics_snapshot
{
    std::array<counter_snapshot, metric_count> counters{};
    std::array<gauge_snapshot, metric_count> gauges{};
    std::array<duration_snapshot, metric_count> durations{};

    [[nodiscard]] const counter_snapshot& counter(metric_id id) const noexcept
    {
        return counters[metric_index(id)];
    }

    [[nodiscard]] const gauge_snapshot& gauge(metric_id id) const noexcept
    {
        return gauges[metric_index(id)];
    }

    [[nodiscard]] const duration_snapshot& duration(metric_id id) const noexcept
    {
        return durations[metric_index(id)];
    }
};

} // namespace cw::server
