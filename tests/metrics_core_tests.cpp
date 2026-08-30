#include "../server_entry/metrics/metric_registry.hpp"
#include "../server_entry/metrics/scoped_timer.hpp"

#include <chrono>
#include <thread>
#include <vector>

namespace
{

bool test_counter_and_gauge()
{
    using namespace cw::server;
    metrics_store metrics;
    metrics.increment(metric_id::server_initializations);
    metrics.increment(metric_id::server_initializations, 4);
    metrics.set(metric_id::server_active_projects, 3);
    metrics.add(metric_id::server_active_projects, 2);
    metrics.add(metric_id::server_active_projects, -7);

    const auto snapshot = metrics.snapshot();
    return snapshot.counter(metric_id::server_initializations).value == 5 &&
           snapshot.gauge(metric_id::server_active_projects).value == -2;
}

bool test_duration_and_timer()
{
    using namespace std::chrono_literals;
    using namespace cw::server;
    metrics_store metrics;
    metrics.record_duration(metric_id::runtime_attach_duration, 10ns);
    metrics.record_duration(metric_id::runtime_attach_duration, 30ns);
    metrics.record_duration(metric_id::runtime_attach_duration, 20ns);

    const auto before_timer = metrics.snapshot().duration(metric_id::runtime_attach_duration);
    if (before_timer.count != 3 || before_timer.total_ns != 60 ||
        before_timer.min_ns != 10 || before_timer.max_ns != 30)
    {
        return false;
    }

    {
        scoped_timer timer{metrics, metric_id::project_initialization_duration};
    }
    return metrics.snapshot().duration(metric_id::project_initialization_duration).count == 1;
}

bool test_registry()
{
    using namespace cw::server;
    return descriptor(metric_id::project_initializations).kind == metric_kind::counter &&
           descriptor(metric_id::project_initializations).name == "project.initializations";
}

bool test_concurrent_counter()
{
    using namespace cw::server;
    metrics_store metrics;
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t increments_per_thread = 25'000;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (std::size_t index = 0; index < thread_count; ++index)
    {
        threads.emplace_back([&metrics] {
            for (std::size_t increment = 0; increment < increments_per_thread; ++increment)
            {
                metrics.increment(metric_id::server_initializations);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    return metrics.snapshot().counter(metric_id::server_initializations).value ==
           thread_count * increments_per_thread;
}

bool performance_sanity()
{
    cw::server::metrics_store metrics;
    for (std::size_t index = 0; index < 1'000'000; ++index)
    {
        metrics.increment(cw::server::metric_id::server_initializations);
    }
    return metrics.snapshot().counter(cw::server::metric_id::server_initializations).value ==
           1'000'000;
}

} // namespace

int main()
{
    return test_counter_and_gauge() && test_duration_and_timer() && test_registry() &&
                   test_concurrent_counter() && performance_sanity()
               ? 0
               : 1;
}
