#pragma once

#include "metrics_snapshot.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace cw::server
{

struct duration_value
{
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> total_ns{0};
    std::atomic<std::uint64_t> min_ns{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> max_ns{0};
};

struct metric_value
{
    std::atomic<std::uint64_t> counter{0};
    std::atomic<std::int64_t> gauge{0};
    duration_value duration;
};

class metrics_store
{
public:
    [[nodiscard]] metrics_mode mode() const noexcept { return mode_; }
    void set_mode(metrics_mode mode) noexcept { mode_ = mode; }
    [[nodiscard]] bool timing_enabled(metric_id) const noexcept
    {
        return mode_ != metrics_mode::off;
    }
    void increment(metric_id id, std::uint64_t amount = 1) noexcept;
    void set(metric_id id, std::int64_t value) noexcept;
    void add(metric_id id, std::int64_t delta) noexcept;
    void record_duration(metric_id id, std::chrono::nanoseconds duration) noexcept;
    void merge_duration(metric_id id, std::uint64_t count, std::uint64_t total_ns,
                        std::uint64_t min_ns, std::uint64_t max_ns) noexcept;

    [[nodiscard]] metrics_snapshot snapshot() const noexcept;

private:
    metrics_mode mode_ = metrics_mode::basic;
    std::array<metric_value, metric_count> values_{};
};

} // namespace cw::server
