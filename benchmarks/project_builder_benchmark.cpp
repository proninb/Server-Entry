#include "../server_entry/project/builder/project_builder.hpp"
#include "../server_entry/project/graph/graph_manager.hpp"
#include "../server_entry/project/parser/parser.hpp"
#include "../server_entry/project/construction/source_publisher.hpp"
#include "../server_entry/metrics/metrics_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace cw::server;
using clock_type = std::chrono::steady_clock;

struct result { double builder_ms{}; double commit_ms{}; };
struct boundary_result
{
    double parser_ms{};
    double canonical_builder_ms{};
    double commit_ms{};
};

struct workload_dimensions
{
    std::size_t sources{};
    std::size_t types{};
    std::size_t entities{};
    std::size_t members{};
    std::size_t enum_values{};
    std::size_t strings{};
};

double milliseconds(clock_type::duration value)
{
    return std::chrono::duration<double, std::milli>(value).count();
}

result median(std::vector<result> values)
{
    const auto middle = values.size() / 2;
    std::ranges::sort(values, {}, &result::builder_ms);
    const auto builder = values[middle].builder_ms;
    std::ranges::sort(values, {}, &result::commit_ms);
    return {builder, values[middle].commit_ms};
}

void require(bool condition)
{
    if (!condition) std::abort();
}

void register_source(graph_manager& manager, const wchar_t* path)
{
    auto transaction = manager.begin_build();
    require(transaction.sources().add(
        std::filesystem::path{path}, project_item_role::source).ok());
    require(transaction.commit().ok());
}

string_id intern(graph_build_transaction& transaction, const std::string& text)
{
    string_id result;
    require(transaction.strings().intern(text, result).ok());
    return result;
}

result run(graph_manager& manager, std::span<const source_fact_batch> batches)
{
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    const auto t0 = clock_type::now();
    require(builder.build(transaction, batches, operation_id{1}, diagnostics).ok());
    const auto t1 = clock_type::now();
    require(transaction.commit().ok());
    const auto t2 = clock_type::now();
    return {milliseconds(t1 - t0), milliseconds(t2 - t1)};
}

void print_dimensions(const workload_dimensions dimensions)
{
    std::cout << " sources=" << dimensions.sources
              << " types=" << dimensions.types
              << " entities=" << dimensions.entities
              << " members=" << dimensions.members
              << " enum_values=" << dimensions.enum_values
              << " strings=" << dimensions.strings;
}

void print(const char* name, result value, workload_dimensions dimensions)
{
    std::cout << name;
    print_dimensions(dimensions);
    std::cout << " builder_ms=" << value.builder_ms
              << " prepare_publish_ms=" << value.commit_ms
              << " total_ms=" << value.builder_ms + value.commit_ms << '\n';
}

std::string enum_source(const std::size_t count, const std::size_t base = 0)
{
    std::string text;
    text.reserve(count * 24);
    for (std::size_t index = 0; index < count; ++index)
        text += "enum E" + std::to_string(base + index) + " : int;\n";
    return text;
}

boundary_result parser_boundary(const std::size_t count)
{
    graph_manager manager;
    require(manager.initialize().ok());
    register_source(manager, LR"(C:\builder-benchmark\parser.cpp)");
    const auto text = enum_source(count);
    source_context context;
    const auto t0 = clock_type::now();
    require(parse_source(
        {source_id{1}, text}, {}, operation_id{2}, context).ok());
    const auto t1 = clock_type::now();
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    const parser_source_fact_batch batch[] = {
        {source_id{1}, &context, context.enums}};
    require(publish_source_facts(
        transaction, batch[0], builder, operation_id{2}, diagnostics).ok());
    const auto t2 = clock_type::now();
    require(transaction.commit().ok());
    const auto t3 = clock_type::now();
    return {milliseconds(t1 - t0), milliseconds(t2 - t1), milliseconds(t3 - t2)};
}

boundary_result parser_workers()
{
    constexpr std::size_t workers = 4;
    graph_manager manager;
    require(manager.initialize().ok());
    const wchar_t* paths[] = {
        LR"(C:\builder-benchmark\worker0.cpp)",
        LR"(C:\builder-benchmark\worker1.cpp)",
        LR"(C:\builder-benchmark\worker2.cpp)",
        LR"(C:\builder-benchmark\worker3.cpp)"};
    for (const auto* path : paths) register_source(manager, path);
    std::array<std::string, workers> texts;
    std::array<source_context, workers> contexts;
    for (std::size_t index = 0; index < workers; ++index)
        texts[index] = enum_source(2500, index * 2500);
    const auto t0 = clock_type::now();
    std::array<std::thread, workers> threads;
    for (std::size_t index = 0; index < workers; ++index)
        threads[index] = std::thread([&, index]
        {
            require(parse_source(
                {source_id{static_cast<std::uint32_t>(index + 1)}, texts[index]},
                {}, operation_id{3}, contexts[index]).ok());
        });
    for (auto& thread : threads) thread.join();
    const auto t1 = clock_type::now();
    std::array<parser_source_fact_batch, workers> batches;
    for (std::size_t index = 0; index < workers; ++index)
        batches[index] = {
            source_id{static_cast<std::uint32_t>(index + 1)},
            &contexts[index], contexts[index].enums};
    auto transaction = manager.begin_build();
    project_builder builder;
    diagnostic_buffer diagnostics;
    for (const auto& batch : batches)
        require(publish_source_facts(
            transaction, batch, builder, operation_id{3}, diagnostics).ok());
    const auto t2 = clock_type::now();
    require(transaction.commit().ok());
    const auto t3 = clock_type::now();
    return {milliseconds(t1 - t0), milliseconds(t2 - t1), milliseconds(t3 - t2)};
}

void print_boundary(const char* name, const boundary_result value,
                    workload_dimensions dimensions)
{
    std::cout << name;
    print_dimensions(dimensions);
    std::cout << " parser_name_production_ms=" << value.parser_ms
              << " ordered_canonicalization_builder_ms=" << value.canonical_builder_ms
              << " prepare_publish_ms=" << value.commit_ms
              << " total_ms="
              << value.parser_ms + value.canonical_builder_ms + value.commit_ms << '\n';
}

result empty_build()
{
    graph_manager manager;
    require(manager.initialize().ok());
    std::vector<result> samples;
    for (int index = 0; index < 9; ++index) samples.push_back(run(manager, {}));
    return median(std::move(samples));
}

result named_build(std::size_t count)
{
    std::vector<result> samples;
    for (int sample = 0; sample < 7; ++sample)
    {
        graph_manager manager;
        require(manager.initialize().ok());
        register_source(manager, LR"(C:\builder-benchmark\named.cpp)");
        auto transaction = manager.begin_build();
        std::vector<enum_source_fact> facts;
        facts.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
            facts.push_back({intern(transaction, "E" + std::to_string(index)), false,
                             false, enum_definition_state::opaque,
                             builtin_type::integer, {}});
        project_builder builder;
        diagnostic_buffer diagnostics;
        const source_fact_batch batch[] = {{source_id{1}, facts}};
        const auto t0 = clock_type::now();
        require(builder.build(transaction, batch, operation_id{1}, diagnostics).ok());
        const auto t1 = clock_type::now();
        require(transaction.commit().ok());
        const auto t2 = clock_type::now();
        samples.push_back({milliseconds(t1 - t0), milliseconds(t2 - t1)});
    }
    return median(std::move(samples));
}

result incremental_over_100k()
{
    graph_manager manager;
    require(manager.initialize().ok());
    {
        auto transaction = manager.begin_build();
        require(transaction.sources().add(
            LR"(C:\builder-benchmark\large.cpp)", project_item_role::source).ok());
        require(transaction.sources().add(
            LR"(C:\builder-benchmark\incremental.cpp)", project_item_role::source).ok());
        std::vector<enum_source_fact> facts;
        facts.reserve(100000);
        for (std::size_t index = 0; index < 100000; ++index)
            facts.push_back({intern(transaction, "Large" + std::to_string(index)), false,
                             false, enum_definition_state::opaque,
                             builtin_type::integer, {}});
        const source_fact_batch batch[] = {{source_id{1}, facts}};
        project_builder builder;
        diagnostic_buffer diagnostics;
        require(builder.build(transaction, batch, operation_id{1}, diagnostics).ok());
        require(transaction.commit().ok());
    }

    string_id incremental_name;
    {
        auto transaction = manager.begin_build();
        incremental_name = intern(transaction, "Incremental");
        require(transaction.commit().ok());
    }
    const enum_source_fact fact[] = {{incremental_name, false, false,
        enum_definition_state::opaque, builtin_type::integer, {}}};
    const source_fact_batch batch[] = {{source_id{2}, fact}};
    std::vector<result> samples;
    for (int index = 0; index < 9; ++index) samples.push_back(run(manager, batch));
    return median(std::move(samples));
}

result aggregate_members(std::size_t type_count,std::size_t members_per_type,bool references)
{
    graph_manager manager;require(manager.initialize().ok());
    register_source(manager,LR"(C:\builder-benchmark\members.cpp)");
    auto transaction=manager.begin_build();
    std::vector<aggregate_source_fact::member_fact> members;members.reserve(members_per_type);
    for(std::size_t m=0;m<members_per_type;++m)
        members.push_back({intern(transaction,"M"+std::to_string(m)),builtin_type::integer,{},references});
    std::vector<aggregate_source_fact> facts;facts.reserve(type_count);
    for(std::size_t t=0;t<type_count;++t)
        facts.push_back({intern(transaction,"T"+std::to_string(t)),aggregate_definition_state::defined,members});
    project_builder builder;diagnostic_buffer diagnostics;
    const source_fact_batch batch[]={{source_id{1},{},facts}};
    const auto t0=clock_type::now();require(builder.build(transaction,batch,operation_id{5},diagnostics).ok());
    const auto t1=clock_type::now();require(transaction.commit().ok());const auto t2=clock_type::now();
    return{milliseconds(t1-t0),milliseconds(t2-t1)};
}

void prepare_publish_variance_audit()
{
    std::vector<result> plain, refs;
    plain.reserve(10); refs.reserve(10);
    // Warm up both paths, then alternate to reduce order/cache bias.
    (void)aggregate_members(100000, 10, false);
    (void)aggregate_members(100000, 10, true);
    for (int i = 0; i < 10; ++i)
    {
        plain.push_back(aggregate_members(100000, 10, false));
        refs.push_back(aggregate_members(100000, 10, true));
    }
    const auto report = [](const char* label, std::vector<result> values)
    {
        auto summarize = [](std::vector<double> v)
        {
            std::ranges::sort(v);
            double sum = 0; for (const auto x : v) sum += x;
            return std::array<double,4>{v.front(), v[v.size()/2], sum/v.size(), v.back()};
        };
        std::vector<double> b, p, t; for (const auto& x : values) { b.push_back(x.builder_ms); p.push_back(x.commit_ms); t.push_back(x.builder_ms+x.commit_ms); }
        const auto B=summarize(b), P=summarize(p), T=summarize(t);
        std::cout << "variance_audit " << label
                  << " builder[min,median,mean,max]=" << B[0] << ',' << B[1] << ',' << B[2] << ',' << B[3]
                  << " prepare_publish[min,median,mean,max]=" << P[0] << ',' << P[1] << ',' << P[2] << ',' << P[3]
                  << " total[min,median,mean,max]=" << T[0] << ',' << T[1] << ',' << T[2] << ',' << T[3] << '\n';
    };
    report("int", std::move(plain)); report("int_ref", std::move(refs));
}

void compiled_persistence_100k()
{
    graph_manager saved;require(saved.initialize().ok());register_source(saved,LR"(C:\builder-benchmark\compiled.cpp)");
    auto transaction=saved.begin_build();std::vector<enum_source_fact> facts;facts.reserve(100000);
    for(std::size_t i=0;i<100000;++i)facts.push_back({intern(transaction,"Persisted"+std::to_string(i)),false,false,enum_definition_state::opaque,builtin_type::integer,{}});
    project_builder builder;diagnostic_buffer diagnostics;const source_fact_batch batch[]={{source_id{1},facts}};require(builder.build(transaction,batch,operation_id{4},diagnostics).ok());require(transaction.commit().ok());
    const auto file=std::filesystem::temp_directory_path()/L"cw_compiled_100k.bin";std::error_code ignored;std::filesystem::remove(file,ignored);metrics_store metrics;metrics.set_mode(metrics_mode::detailed);
    require(saved.save_compiled_checkpoint(file,&metrics).ok());graph_manager loaded;require(loaded.initialize().ok());require(loaded.load_compiled_checkpoint(file,&metrics).ok());const auto snapshot=metrics.snapshot();
    const auto ms=[&](metric_id id){return snapshot.duration(id).total_ns/1000000.0;};
    std::cout<<"compiled_persistence";
    print_dimensions({1,100000,100000,0,0,100000});
    std::cout<<" artifact_bytes="<<std::filesystem::file_size(file)
             <<" save_ms="<<ms(metric_id::compiled_save_total_duration)
             <<" load_ms="<<ms(metric_id::compiled_load_total_duration)
             <<" validate_ms="<<ms(metric_id::compiled_load_validate_duration)
             <<" strings_ms="<<ms(metric_id::compiled_load_strings_duration)
             <<" graph_ms="<<ms(metric_id::compiled_load_graph_duration)<<'\n';
    std::filesystem::remove(file,ignored);
}
} // namespace

int main()
{
    print("empty", empty_build(), {});
    print("named_1", named_build(1), {1,1,1,0,0,1});
    print("named_10000", named_build(10000), {1,10000,10000,0,0,10000});
    // The measured operation replaces one Source over a committed baseline of
    // 100,000 named opaque enums. Report the total project shape, not only K=1.
    print("k1_over_100000", incremental_over_100k(),
          {2,100001,100001,0,0,100001});
    print_boundary("parser_boundary_1", parser_boundary(1), {1,1,1,0,0,1});
    print_boundary("parser_boundary_10000", parser_boundary(10000),
                   {1,10000,10000,0,0,10000});
    print_boundary("parser_boundary_100000", parser_boundary(100000),
                   {1,100000,100000,0,0,100000});
    print_boundary("parser_workers_4x2500", parser_workers(),
                   {4,10000,10000,0,0,10000});
    print("members_100k_types_x1_int",aggregate_members(100000,1,false),
          {1,100000,100000,100000,0,100001});
    print("members_100k_types_x10_int",aggregate_members(100000,10,false),
          {1,100000,100000,1000000,0,100010});
    print("members_100k_types_x10_int_ref",aggregate_members(100000,10,true),
          {1,100000,100000,1000000,0,100010});
    prepare_publish_variance_audit();
    compiled_persistence_100k();
}
