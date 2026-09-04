#pragma once

#include "metrics_store.hpp"

#include <chrono>

namespace cw::server {

// Measures one scoped operation and records its elapsed time through Recorder.
// Timing is enabled once at construction, and the duration is committed
// automatically on destruction using std::chrono::steady_clock.
template <typename Recorder>
class basic_scoped_timer {
public:
    basic_scoped_timer(Recorder& metrics, metric_id id) noexcept
        : metrics(metrics), id(id), enabled(metrics.timing_enabled(id)) {
        if (enabled) {
            start = std::chrono::steady_clock::now();
        }
    }

    basic_scoped_timer(const basic_scoped_timer&) = delete;
    basic_scoped_timer& operator=(const basic_scoped_timer&) = delete;

    ~basic_scoped_timer() noexcept {
        if (enabled) {
            metrics.record_duration(
                id,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start));
        }
    }

private:
    Recorder& metrics;
    metric_id id;
    bool enabled = false;
    std::chrono::steady_clock::time_point start;
};

// Default scoped timer used with the process-wide metrics_store.
using scoped_timer = basic_scoped_timer<metrics_store>;

} // namespace cw::server
