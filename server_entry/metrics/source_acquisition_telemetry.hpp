#pragma once

#include "metrics_store.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>

namespace cw::server {

#ifdef CW_METRICS_TEST_CLOCK

inline std::uint64_t source_telemetry_test_clock_reads = 0;

inline void reset_source_telemetry_test_clock() noexcept {
    source_telemetry_test_clock_reads = 0;
}

[[nodiscard]] inline std::uint64_t source_telemetry_test_clock_read_count() noexcept {
    return source_telemetry_test_clock_reads;
}

#endif

// Provides the monotonic clock used by Source acquisition timing.
// The indirection allows timing tests to count clock reads without changing
// production timing behavior.
struct source_telemetry_clock {
    using time_point = std::chrono::steady_clock::time_point;

    [[nodiscard]] static time_point now() noexcept {
#ifdef CW_METRICS_TEST_CLOCK
        ++source_telemetry_test_clock_reads;
#endif
        return std::chrono::steady_clock::now();
    }
};

class source_acquisition_timing;

// Accumulates telemetry locally for one Source acquisition operation.
// The combiner avoids atomic writes on the hot path and flushes aggregated
// counters and durations into the authoritative process-wide metrics_store.
class source_acquisition_telemetry {
public:
    explicit source_acquisition_telemetry(
        metrics_mode mode = metrics_mode::basic) noexcept
        : current_mode(mode) {}

    [[nodiscard]] metrics_mode mode() const noexcept { return current_mode; }

    void increment(metric_id id, std::uint64_t amount = 1) noexcept {
        if (current_mode == metrics_mode::off) {
            return;
        }

        switch (id) {
        case metric_id::source_acquisition_count:
            attempts += amount;
            break;

        case metric_id::source_unchanged_fast_path_count:
            fast_paths += amount;
            break;

        case metric_id::source_content_read_count:
            reads += amount;
            break;

        case metric_id::source_acquisition_failure_count:
            failures += amount;
            break;

        case metric_id::source_toctou_rejection_count:
            toctou += amount;
            break;

        case metric_id::source_bytes_read:
            bytes_read += amount;
            break;

        default:
            assert(false);
            break;
        }
    }

    void record_duration(metric_id id, std::chrono::nanoseconds duration) noexcept {
        if (current_mode != metrics_mode::detailed) {
            return;
        }

        const auto first = metric_index(metric_id::source_acquisition_duration);
        const auto index = metric_index(id);

        // These seven duration metrics form one contiguous metric_id range.
        assert(index >= first && index < first + durations.size());
        assert(duration.count() >= 0);

        if (index < first ||
            index >= first + durations.size() ||
            duration.count() < 0) {
            return;
        }

        const auto value = static_cast<std::uint64_t>(duration.count());
        auto& target = durations[index - first];

        ++target.count;
        target.total_ns += value;
        target.min_ns = (std::min)(target.min_ns, value);
        target.max_ns = (std::max)(target.max_ns, value);
    }

    // Merges another worker-local acquisition accumulator into this one.
    // This is used after parallel Source discovery so no telemetry atomics are
    // needed on the filesystem/hash hot path.
    void merge_from(
        const source_acquisition_telemetry& other) noexcept {

        if (current_mode == metrics_mode::off) {
            return;
        }

        attempts += other.attempts;
        fast_paths += other.fast_paths;
        reads += other.reads;
        failures += other.failures;
        toctou += other.toctou;
        bytes_read += other.bytes_read;

        if (current_mode != metrics_mode::detailed) {
            return;
        }

        for (std::size_t index = 0; index < durations.size(); ++index) {
            const auto& source = other.durations[index];

            if (source.count == 0) {
                continue;
            }

            auto& destination = durations[index];
            destination.count += source.count;
            destination.total_ns += source.total_ns;
            destination.min_ns = (std::min)(
                destination.min_ns,
                source.min_ns);
            destination.max_ns = (std::max)(
                destination.max_ns,
                source.max_ns);
        }
    }

    void flush_to(metrics_store& destination) noexcept {
        if (current_mode == metrics_mode::off) {
            return;
        }

        merge_counter(
            destination,
            metric_id::source_acquisition_count,
            attempts);

        merge_counter(
            destination,
            metric_id::source_unchanged_fast_path_count,
            fast_paths);

        merge_counter(
            destination,
            metric_id::source_content_read_count,
            reads);

        merge_counter(
            destination,
            metric_id::source_acquisition_failure_count,
            failures);

        merge_counter(
            destination,
            metric_id::source_toctou_rejection_count,
            toctou);

        merge_counter(
            destination,
            metric_id::source_bytes_read,
            bytes_read);

        const auto first = metric_index(metric_id::source_acquisition_duration);

        for (std::size_t index = 0; index < durations.size(); ++index) {
            const auto& duration = durations[index];

            if (duration.count != 0) {
                destination.merge_duration(
                    static_cast<metric_id>(first + index),
                    duration.count,
                    duration.total_ns,
                    duration.min_ns,
                    duration.max_ns);
            }
        }

        const auto active_mode = current_mode;
        *this = source_acquisition_telemetry{active_mode};
    }

private:
    friend class source_acquisition_timing;

    struct duration_aggregate {
        std::uint64_t count = 0;
        std::uint64_t total_ns = 0;
        std::uint64_t min_ns = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t max_ns = 0;
    };

    static void merge_counter(
        metrics_store& destination,
        metric_id id,
        std::uint64_t value) noexcept {

        if (value != 0) {
            destination.increment(id, value);
        }
    }

    std::array<duration_aggregate, 7> durations{};
    metrics_mode current_mode = metrics_mode::basic;
    std::uint64_t attempts = 0;
    std::uint64_t fast_paths = 0;
    std::uint64_t reads = 0;
    std::uint64_t failures = 0;
    std::uint64_t toctou = 0;
    std::uint64_t bytes_read = 0;
};

// Measures the synchronous phases of one Source acquisition operation.
// Phase durations are recorded between shared clock boundaries, while total
// duration spans the complete classified acquisition interval.
class source_acquisition_timing {
public:
    explicit source_acquisition_timing(
        source_acquisition_telemetry& telemetry) noexcept
        : telemetry(telemetry),
          current_mode(telemetry.mode()) {

        if (current_mode == metrics_mode::detailed) {
            start = source_telemetry_clock::now();
            boundary = start;
        }
    }

    source_acquisition_timing(const source_acquisition_timing&) = delete;
    source_acquisition_timing& operator=(const source_acquisition_timing&) = delete;

    ~source_acquisition_timing() noexcept {
        if (current_mode != metrics_mode::detailed) {
            return;
        }

        telemetry.record_duration(
            metric_id::source_acquisition_duration,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                boundary - start));
    }

    void finish_phase(metric_id id) noexcept {
        if (current_mode != metrics_mode::detailed) {
            return;
        }

        const auto next = source_telemetry_clock::now();

        telemetry.record_duration(
            id,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                next - boundary));

        boundary = next;
    }

    // Advances total acquisition time across work that is intentionally not
    // attributed to either adjacent measured phase.
    void advance_boundary() noexcept {
        if (current_mode == metrics_mode::detailed) {
            boundary = source_telemetry_clock::now();
        }
    }

private:
    source_acquisition_telemetry& telemetry;
    metrics_mode current_mode = metrics_mode::off;
    source_telemetry_clock::time_point start{};
    source_telemetry_clock::time_point boundary{};
};

} // namespace cw::server
