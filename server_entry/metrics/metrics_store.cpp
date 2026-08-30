#include "metrics_store.hpp"

#include "metric_registry.hpp"

#include <cassert>

namespace cw::server
{
namespace
{

void update_min(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept
{
    auto current = target.load(std::memory_order_relaxed);
    while (value < current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
    {
    }
}

void update_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept
{
    auto current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
    {
    }
}

} // namespace

void metrics_store::increment(metric_id id, std::uint64_t amount) noexcept
{
    assert(descriptor(id).kind == metric_kind::counter);
    if (descriptor(id).kind != metric_kind::counter)
    {
        return;
    }
    values_[metric_index(id)].counter.fetch_add(amount, std::memory_order_relaxed);
}

void metrics_store::set(metric_id id, std::int64_t value) noexcept
{
    assert(descriptor(id).kind == metric_kind::gauge);
    if (descriptor(id).kind != metric_kind::gauge)
    {
        return;
    }
    values_[metric_index(id)].gauge.store(value, std::memory_order_relaxed);
}

void metrics_store::add(metric_id id, std::int64_t delta) noexcept
{
    assert(descriptor(id).kind == metric_kind::gauge);
    if (descriptor(id).kind != metric_kind::gauge)
    {
        return;
    }
    values_[metric_index(id)].gauge.fetch_add(delta, std::memory_order_relaxed);
}

void metrics_store::record_duration(metric_id id, std::chrono::nanoseconds duration) noexcept
{
    assert(descriptor(id).kind == metric_kind::duration);
    assert(duration.count() >= 0);
    if (descriptor(id).kind != metric_kind::duration || duration.count() < 0)
    {
        return;
    }

    const auto value = static_cast<std::uint64_t>(duration.count());
    auto& target = values_[metric_index(id)].duration;
    target.count.fetch_add(1, std::memory_order_relaxed);
    target.total_ns.fetch_add(value, std::memory_order_relaxed);
    update_min(target.min_ns, value);
    update_max(target.max_ns, value);
}

metrics_snapshot metrics_store::snapshot() const noexcept
{
    metrics_snapshot result;
    for (std::size_t index = 0; index < metric_count; ++index)
    {
        const auto& source = values_[index];
        result.counters[index].value = source.counter.load(std::memory_order_relaxed);
        result.gauges[index].value = source.gauge.load(std::memory_order_relaxed);

        auto& duration = result.durations[index];
        duration.count = source.duration.count.load(std::memory_order_relaxed);
        duration.total_ns = source.duration.total_ns.load(std::memory_order_relaxed);
        duration.min_ns = duration.count == 0
                              ? 0
                              : source.duration.min_ns.load(std::memory_order_relaxed);
        duration.max_ns = source.duration.max_ns.load(std::memory_order_relaxed);
    }
    return result;
}

} // namespace cw::server
