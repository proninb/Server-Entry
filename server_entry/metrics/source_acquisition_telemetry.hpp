#pragma once

#include "metrics_store.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>

namespace cw::server
{

#ifdef CW_METRICS_TEST_CLOCK
inline std::uint64_t source_telemetry_test_clock_reads = 0;
inline void reset_source_telemetry_test_clock() noexcept
{
    source_telemetry_test_clock_reads = 0;
}
[[nodiscard]] inline std::uint64_t source_telemetry_test_clock_read_count() noexcept
{
    return source_telemetry_test_clock_reads;
}
#endif

struct source_telemetry_clock
{
    using time_point = std::chrono::steady_clock::time_point;
    [[nodiscard]] static time_point now() noexcept
    {
#ifdef CW_METRICS_TEST_CLOCK
        ++source_telemetry_test_clock_reads;
#endif
        return std::chrono::steady_clock::now();
    }
};

class source_acquisition_timing;

// Operation-local write combiner for the SM2 acquisition hot path. It has no
// query surface and flushes into the authoritative Metrics Core store.
class source_acquisition_telemetry
{
public:
    explicit source_acquisition_telemetry(
        metrics_mode mode = metrics_mode::basic) noexcept : mode_(mode)
    {
    }

    [[nodiscard]] metrics_mode mode() const noexcept { return mode_; }

    void increment(metric_id id, std::uint64_t amount = 1) noexcept
    {
        if (mode_ == metrics_mode::off) return;
        switch (id)
        {
        case metric_id::source_acquisition_count: attempts_ += amount; break;
        case metric_id::source_unchanged_fast_path_count: fast_paths_ += amount; break;
        case metric_id::source_content_read_count: reads_ += amount; break;
        case metric_id::source_acquisition_failure_count: failures_ += amount; break;
        case metric_id::source_toctou_rejection_count: toctou_ += amount; break;
        case metric_id::source_bytes_read: bytes_read_ += amount; break;
        default: assert(false); break;
        }
    }

    void record_duration(metric_id id, std::chrono::nanoseconds duration) noexcept
    {
        if (mode_ != metrics_mode::detailed) return;
        const auto first = metric_index(metric_id::source_acquisition_duration);
        const auto index = metric_index(id);
        assert(index >= first && index < first + durations_.size());
        assert(duration.count() >= 0);
        if (index < first || index >= first + durations_.size() || duration.count() < 0)
            return;
        const auto value = static_cast<std::uint64_t>(duration.count());
        auto& target = durations_[index - first];
        ++target.count;
        target.total_ns += value;
        target.min_ns = (std::min)(target.min_ns, value);
        target.max_ns = (std::max)(target.max_ns, value);
    }

    void flush_to(metrics_store& destination) noexcept
    {
        if (mode_ == metrics_mode::off) return;
        merge_counter(destination, metric_id::source_acquisition_count, attempts_);
        merge_counter(destination, metric_id::source_unchanged_fast_path_count, fast_paths_);
        merge_counter(destination, metric_id::source_content_read_count, reads_);
        merge_counter(destination, metric_id::source_acquisition_failure_count, failures_);
        merge_counter(destination, metric_id::source_toctou_rejection_count, toctou_);
        merge_counter(destination, metric_id::source_bytes_read, bytes_read_);
        const auto first = metric_index(metric_id::source_acquisition_duration);
        for (std::size_t index = 0; index < durations_.size(); ++index)
        {
            const auto& duration = durations_[index];
            if (duration.count != 0)
                destination.merge_duration(static_cast<metric_id>(first + index),
                                           duration.count, duration.total_ns,
                                           duration.min_ns, duration.max_ns);
        }
        const auto active_mode = mode_;
        *this = source_acquisition_telemetry{active_mode};
    }

private:
    friend class source_acquisition_timing;
    struct duration_aggregate
    {
        std::uint64_t count = 0;
        std::uint64_t total_ns = 0;
        std::uint64_t min_ns = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t max_ns = 0;
    };

    static void merge_counter(metrics_store& destination, metric_id id,
                              std::uint64_t value) noexcept
    {
        if (value != 0) destination.increment(id, value);
    }

    std::array<duration_aggregate, 7> durations_{};
    metrics_mode mode_ = metrics_mode::basic;
    std::uint64_t attempts_ = 0;
    std::uint64_t fast_paths_ = 0;
    std::uint64_t reads_ = 0;
    std::uint64_t failures_ = 0;
    std::uint64_t toctou_ = 0;
    std::uint64_t bytes_read_ = 0;
};

// Shared-boundary timer for the synchronous SM2 acquisition phases. Total
// duration is derived from the first and last phase boundary.
class source_acquisition_timing
{
public:
    explicit source_acquisition_timing(source_acquisition_telemetry& telemetry) noexcept
        : telemetry_(telemetry), mode_(telemetry.mode())
    {
        if (mode_ == metrics_mode::detailed)
        {
            start_ = source_telemetry_clock::now();
            boundary_ = start_;
        }
    }

    source_acquisition_timing(const source_acquisition_timing&) = delete;
    source_acquisition_timing& operator=(const source_acquisition_timing&) = delete;

    ~source_acquisition_timing() noexcept
    {
        if (mode_ != metrics_mode::detailed) return;
        telemetry_.record_duration(
            metric_id::source_acquisition_duration,
            std::chrono::duration_cast<std::chrono::nanoseconds>(boundary_ - start_));
    }

    void finish_phase(metric_id id) noexcept
    {
        if (mode_ != metrics_mode::detailed) return;
        const auto next = source_telemetry_clock::now();
        telemetry_.record_duration(
            id, std::chrono::duration_cast<std::chrono::nanoseconds>(next - boundary_));
        boundary_ = next;
    }

    // Advances total time across unclassified acquisition control work (most
    // notably CloseHandle) without attributing it to an adjacent phase.
    void advance_boundary() noexcept
    {
        if (mode_ == metrics_mode::detailed)
            boundary_ = source_telemetry_clock::now();
    }

private:
    source_acquisition_telemetry& telemetry_;
    metrics_mode mode_ = metrics_mode::off;
    source_telemetry_clock::time_point start_{};
    source_telemetry_clock::time_point boundary_{};
};

} // namespace cw::server
