#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using clock_type = std::chrono::steady_clock;

struct source_core
{
    std::uint32_t path_offset;
    std::uint32_t path_length;
};

struct physical_state
{
    std::uint64_t write_time;
    std::uint64_t file_size;
    std::array<std::byte, 32> sha256;
    std::uint32_t flags;
    std::uint32_t reserved;
};

struct monolithic_source
{
    source_core core;
    physical_state physical;
};

struct bucket32
{
    std::uint32_t hash;
    std::uint32_t source;
};

struct bucket64
{
    std::uint64_t hash;
    std::uint32_t source;
    std::uint32_t reserved;
};

static_assert(sizeof(source_core) == 8);
static_assert(sizeof(physical_state) == 56);
static_assert(sizeof(monolithic_source) == 64);
static_assert(sizeof(bucket32) == 8);
static_assert(sizeof(bucket64) == 16);

volatile std::uint64_t result_sink = 0;

std::uint64_t fnv1a64(std::string_view value) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : value)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t read64(const char* data) noexcept
{
    std::uint64_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::uint32_t read32(const char* data) noexcept
{
    std::uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::uint64_t rotate_left(std::uint64_t value, int count) noexcept
{
    return std::rotl(value, count);
}

std::uint64_t xxh64_round(std::uint64_t accumulator, std::uint64_t input) noexcept
{
    accumulator += input * 14029467366897019727ull;
    accumulator = rotate_left(accumulator, 31);
    return accumulator * 11400714785074694791ull;
}

std::uint64_t xxh64(std::string_view value) noexcept
{
    constexpr std::uint64_t p1 = 11400714785074694791ull;
    constexpr std::uint64_t p2 = 14029467366897019727ull;
    constexpr std::uint64_t p3 = 1609587929392839161ull;
    constexpr std::uint64_t p4 = 9650029242287828579ull;
    constexpr std::uint64_t p5 = 2870177450012600261ull;
    const char* position = value.data();
    const char* const end = position + value.size();
    std::uint64_t hash;
    if (value.size() >= 32)
    {
        std::uint64_t v1 = p1 + p2;
        std::uint64_t v2 = p2;
        std::uint64_t v3 = 0;
        std::uint64_t v4 = 0 - p1;
        const char* const limit = end - 32;
        do
        {
            v1 = xxh64_round(v1, read64(position)); position += 8;
            v2 = xxh64_round(v2, read64(position)); position += 8;
            v3 = xxh64_round(v3, read64(position)); position += 8;
            v4 = xxh64_round(v4, read64(position)); position += 8;
        } while (position <= limit);
        hash = rotate_left(v1, 1) + rotate_left(v2, 7) +
               rotate_left(v3, 12) + rotate_left(v4, 18);
        for (const auto value_to_merge : {v1, v2, v3, v4})
        {
            hash ^= xxh64_round(0, value_to_merge);
            hash = hash * p1 + p4;
        }
    }
    else hash = p5;
    hash += value.size();
    while (position + 8 <= end)
    {
        const auto lane = xxh64_round(0, read64(position));
        hash ^= lane;
        hash = rotate_left(hash, 27) * p1 + p4;
        position += 8;
    }
    if (position + 4 <= end)
    {
        hash ^= static_cast<std::uint64_t>(read32(position)) * p1;
        hash = rotate_left(hash, 23) * p2 + p3;
        position += 4;
    }
    while (position < end)
    {
        hash ^= static_cast<unsigned char>(*position++) * p5;
        hash = rotate_left(hash, 11) * p1;
    }
    hash ^= hash >> 33;
    hash *= p2;
    hash ^= hash >> 29;
    hash *= p3;
    return hash ^ (hash >> 32);
}

std::uint32_t fold32(std::uint64_t value) noexcept
{
    auto folded = static_cast<std::uint32_t>(value ^ (value >> 32));
    return folded == 0 ? 1u : folded;
}

std::size_t index_capacity(std::size_t count)
{
    return std::bit_ceil((count * 10 + 6) / 7);
}

struct path_data
{
    std::vector<std::string> paths;
    std::vector<std::string> misses;
    std::vector<std::uint32_t> random_order;
};

path_data make_paths(std::size_t count)
{
    path_data output;
    output.paths.reserve(count);
    output.misses.reserve(count);
    output.random_order.resize(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto group = index / 1000;
        const auto leaf = index % 1000;
        output.paths.push_back("C:/server-entry/project/generated/common/prefix/module_" +
                               std::to_string(group) + "/source_" +
                               std::to_string(leaf) + ".noc");
        output.misses.push_back(output.paths.back() + ".missing");
        output.random_order[index] = static_cast<std::uint32_t>(index);
    }
    std::mt19937 generator{0x534d4231u};
    std::ranges::shuffle(output.random_order, generator);
    return output;
}

template<class Bucket, class Hash>
class path_index
{
public:
    path_index(std::span<const std::string> paths, Hash hash)
        : buckets_(index_capacity(paths.size())), hash_(hash), mask_(buckets_.size() - 1)
    {
        for (std::size_t index = 0; index < paths.size(); ++index)
            insert(paths[index], static_cast<std::uint32_t>(index + 1));
    }

    std::uint32_t find(std::string_view value, std::span<const std::string> paths,
                       std::uint64_t& probes) const noexcept
    {
        const auto full_hash = hash_(value);
        const auto stored_hash = bucket_hash(full_hash);
        auto position = static_cast<std::size_t>(full_hash) & mask_;
        for (;;)
        {
            ++probes;
            const auto& bucket = buckets_[position];
            if (bucket.source == 0) return 0;
            if (bucket.hash == stored_hash && paths[bucket.source - 1] == value)
                return bucket.source;
            position = (position + 1) & mask_;
        }
    }

    std::size_t bytes() const noexcept { return buckets_.size() * sizeof(Bucket); }
    std::size_t capacity() const noexcept { return buckets_.size(); }

private:
    using hash_field = decltype(Bucket::hash);
    static hash_field bucket_hash(std::uint64_t hash) noexcept
    {
        if constexpr (sizeof(hash_field) == 4) return fold32(hash);
        else return hash == 0 ? 1 : hash;
    }

    void insert(std::string_view value, std::uint32_t source)
    {
        const auto full_hash = hash_(value);
        auto position = static_cast<std::size_t>(full_hash) & mask_;
        while (buckets_[position].source != 0) position = (position + 1) & mask_;
        buckets_[position].hash = bucket_hash(full_hash);
        buckets_[position].source = source;
    }

    std::vector<Bucket> buckets_;
    Hash hash_;
    std::size_t mask_;
};

template<class Function>
double median_ms(Function&& function, int samples = 7)
{
    std::vector<double> values;
    values.reserve(samples);
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto begin = clock_type::now();
        result_sink = result_sink ^ function();
        const auto end = clock_type::now();
        values.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    std::ranges::sort(values);
    return values[values.size() / 2];
}

template<class Index>
void benchmark_index(std::string_view name, const Index& index, const path_data& data)
{
    const auto run = [&](std::span<const std::string> queries,
                         std::span<const std::uint32_t> order)
    {
        std::uint64_t probes = 0;
        std::uint64_t found = 0;
        for (const auto position : order)
            found += index.find(queries[position], data.paths, probes) != 0;
        return std::pair{found, probes};
    };
    std::vector<std::uint32_t> sequential(data.paths.size());
    std::iota(sequential.begin(), sequential.end(), 0u);
    const auto measure = [&](const char* kind,
                             const std::vector<std::string>& queries,
                             const std::vector<std::uint32_t>& order)
    {
        std::uint64_t probes = 0;
        const auto measured = median_ms([&]
        {
            const auto [found, current_probes] = run(queries, order);
            probes = current_probes;
            return found + current_probes;
        });
        std::cout << "index=" << name << " workload=" << kind
                  << " median_ms=" << measured
                  << " ns_per_lookup=" << measured * 1'000'000.0 / data.paths.size()
                  << " probes_per_lookup=" << static_cast<double>(probes) / data.paths.size()
                  << '\n';
    };
    measure("hit_sequential", data.paths, sequential);
    measure("hit_random", data.paths, data.random_order);
    measure("miss_random", data.misses, data.random_order);
    std::cout << "index=" << name << " bucket_bytes="
              << index.bytes() / (1024.0 * 1024.0)
              << " capacity=" << index.capacity() << '\n';
}

void benchmark_layout(std::size_t count, const path_data& data)
{
    std::vector<monolithic_source> monolithic(count);
    std::vector<source_core> cores(count);
    std::vector<physical_state> physical(count);
    std::uint32_t offset = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const source_core core{offset, static_cast<std::uint32_t>(data.paths[index].size())};
        offset += core.path_length;
        monolithic[index].core = core;
        monolithic[index].physical.file_size = index;
        cores[index] = core;
        physical[index].file_size = index;
    }
    const auto hot = [](const auto& values)
    {
        std::uint64_t total = 0;
        for (const auto& value : values)
        {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, source_core>)
                total += value.path_offset + value.path_length;
            else total += value.core.path_offset + value.core.path_length;
        }
        return total;
    };
    const auto cold_monolithic = [&]
    {
        std::uint64_t total = 0;
        for (const auto& value : monolithic) total += value.physical.file_size;
        return total;
    };
    const auto cold_split = [&]
    {
        std::uint64_t total = 0;
        for (const auto& value : physical) total += value.file_size;
        return total;
    };
    std::cout << "layout=monolithic hot_scan_ms=" << median_ms([&]{ return hot(monolithic); })
              << " cold_scan_ms=" << median_ms(cold_monolithic)
              << " bytes=" << monolithic.size() * sizeof(monolithic_source) << '\n';
    std::cout << "layout=split hot_scan_ms=" << median_ms([&]{ return hot(cores); })
              << " cold_scan_ms=" << median_ms(cold_split)
              << " core_bytes=" << cores.size() * sizeof(source_core)
              << " physical_bytes=" << physical.size() * sizeof(physical_state) << '\n';
}

struct fnv_hash { std::uint64_t operator()(std::string_view value) const noexcept { return fnv1a64(value); } };
struct xxh_hash { std::uint64_t operator()(std::string_view value) const noexcept { return xxh64(value); } };

void run(std::size_t count)
{
    std::cout << std::fixed << std::setprecision(3) << "sources=" << count << '\n';
    const auto data = make_paths(count);
    benchmark_layout(count, data);
    benchmark_index("fnv1a32_bucket8", path_index<bucket32, fnv_hash>{data.paths, {}}, data);
    benchmark_index("xxh32_bucket8", path_index<bucket32, xxh_hash>{data.paths, {}}, data);
    benchmark_index("fnv1a64_bucket16", path_index<bucket64, fnv_hash>{data.paths, {}}, data);
    benchmark_index("xxh64_bucket16", path_index<bucket64, xxh_hash>{data.paths, {}}, data);
}
} // namespace

int main(int argc, char** argv)
{
    if (xxh64({}) != 0xef46db3751d8e999ull) return 3;
    if (argc == 2)
    {
        const auto count = std::strtoull(argv[1], nullptr, 10);
        if (count == 0 || count > std::numeric_limits<std::uint32_t>::max() - 1) return 2;
        run(static_cast<std::size_t>(count));
        return 0;
    }
    run(100'000);
    run(1'000'000);
}
