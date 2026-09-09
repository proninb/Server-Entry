#include "../server_entry/project/parser/parser.hpp"
#include "../server_entry/project/parser/source_environment.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace cw::server;
using clock_type = std::chrono::steady_clock;

constexpr std::array<std::size_t, 4> sizes{
    1024,
    4096,
    16384,
    32768
};

constexpr int warmup_runs = 2;
constexpr int measured_runs = 7;
constexpr std::size_t lookup_repetitions = 16384;
constexpr std::size_t mixed_key_count = 64;

volatile std::uint64_t sink = 0;

std::string make_source(std::size_t count) {
    std::string text;
    text.reserve(64 + count * 32);

    text += "enum E : int {";

    for (std::size_t index = 0;
         index != count;
         ++index) {

        text += 'C';
        text += std::to_string(index);
        text += '=';
        text += std::to_string(index);
        text += ',';
    }

    text += "};";

    for (std::size_t index = 0;
         index != count;
         ++index) {

        text += "struct T";
        text += std::to_string(index);
        text += ';';
    }

    return text;
}

bool parse_fixture(
    std::size_t count,
    source_context& context) {

    const auto text =
        make_source(count);

    source_environment environment;

    const auto result =
        parse_source(
            source_view{
                source_id{1},
                text
            },
            environment,
            operation_id{1},
            context);

    return
        result.ok() &&
        context.diagnostics.empty() &&
        context.enum_values.size() == count &&
        context.type_declarations.size() == count + 1;
}

template <typename Function>
double timed(Function&& function) {
    const auto begin =
        clock_type::now();

    function();

    const auto end =
        clock_type::now();

    return std::chrono::duration<double, std::milli>(
        end - begin).count();
}

double initialize_once(
    source_context& context) {

    source_interface_storage storage;
    bool ok = false;

    const auto elapsed =
        timed([&]() {
            ok =
                storage.initialize(
                    source_id{1},
                    context).ok();
        });

    if (!ok) {
        return -1.0;
    }

    return elapsed;
}

template <std::size_t Count>
double constant_lookup(
    const source_environment& environment,
    const std::array<std::string, Count>& keys) {

    std::uint64_t local = 0;

    const auto elapsed =
        timed([&]() {
            for (std::size_t index = 0;
                 index != lookup_repetitions;
                 ++index) {

                integral_constant value;

                const auto result =
                    environment.find_constant_exact(
                        {},
                        keys[index % Count],
                        value);

                local +=
                    result.ok()
                        ? value.bits + 1
                        : 17;
            }
        });

    sink += local;
    return elapsed;
}

template <std::size_t Count>
double type_lookup(
    const source_environment& environment,
    const std::array<std::string, Count>& keys) {

    std::uint64_t local = 0;

    const auto elapsed =
        timed([&]() {
            for (std::size_t index = 0;
                 index != lookup_repetitions;
                 ++index) {

                source_entity_ref entity;
                std::string_view canonical;

                const auto result =
                    environment.find_type_exact(
                        {},
                        keys[index % Count],
                        (std::numeric_limits<std::uint32_t>::max)(),
                        entity,
                        canonical);

                local +=
                    result.ok()
                        ? entity.declaration.value() +
                              canonical.size()
                        : 19;
            }
        });

    sink += local;
    return elapsed;
}

template <typename Function>
void emit_lookup(
    std::size_t count,
    std::string_view kind,
    std::string_view shape,
    Function&& function) {

    for (int run = 0;
         run != warmup_runs;
         ++run) {
        (void)function();
    }

    for (int run = 0;
         run != measured_runs;
         ++run) {

        const auto elapsed =
            function();

        std::cout
            << count
            << ",lookup,"
            << kind
            << ','
            << shape
            << ','
            << run
            << ','
            << elapsed
            << ','
            << lookup_repetitions
            << '\n';
    }
}

} // namespace

int main() {
    std::cout
        << std::fixed
        << std::setprecision(6)
        << "interface_size,phase,kind,shape,run,ms,operations\n";

    for (const auto count : sizes) {
        source_context context;

        if (!parse_fixture(
                count,
                context)) {
            std::cerr
                << "parse fixture failed at "
                << count
                << '\n';

            return 1;
        }

        for (int run = 0;
             run != warmup_runs;
             ++run) {

            if (initialize_once(context) < 0.0) {
                return 1;
            }
        }

        for (int run = 0;
             run != measured_runs;
             ++run) {

            const auto elapsed =
                initialize_once(context);

            if (elapsed < 0.0) {
                return 1;
            }

            std::cout
                << count
                << ",initialize,interface,build,"
                << run
                << ','
                << elapsed
                << ",1\n";
        }

        source_interface_storage storage;

        if (!storage.initialize(
                source_id{1},
                context).ok()) {
            return 1;
        }

        source_environment environment{
            storage
        };

        std::array<std::string, mixed_key_count>
            constant_hits;

        std::array<std::string, mixed_key_count>
            constant_misses;

        std::array<std::string, mixed_key_count>
            type_hits;

        std::array<std::string, mixed_key_count>
            type_misses;

        for (std::size_t index = 0;
             index != mixed_key_count;
             ++index) {

            const auto coordinate =
                (count - 1) * index /
                (mixed_key_count - 1);

            constant_hits[index] =
                "C" +
                std::to_string(
                    coordinate);

            constant_misses[index] =
                "MissingC" +
                std::to_string(
                    index);

            type_hits[index] =
                "T" +
                std::to_string(
                    coordinate);

            type_misses[index] =
                "MissingT" +
                std::to_string(
                    index);
        }

        const std::array<std::string, 1>
            constant_last{
                "C" +
                std::to_string(
                    count - 1)
            };

        const std::array<std::string, 1>
            type_last{
                "T" +
                std::to_string(
                    count - 1)
            };

        emit_lookup(
            count,
            "constant",
            "mixed_hit",
            [&]() {
                return constant_lookup(
                    environment,
                    constant_hits);
            });

        emit_lookup(
            count,
            "constant",
            "mixed_miss",
            [&]() {
                return constant_lookup(
                    environment,
                    constant_misses);
            });

        emit_lookup(
            count,
            "constant",
            "hot_last",
            [&]() {
                return constant_lookup(
                    environment,
                    constant_last);
            });

        emit_lookup(
            count,
            "type",
            "mixed_hit",
            [&]() {
                return type_lookup(
                    environment,
                    type_hits);
            });

        emit_lookup(
            count,
            "type",
            "mixed_miss",
            [&]() {
                return type_lookup(
                    environment,
                    type_misses);
            });

        emit_lookup(
            count,
            "type",
            "hot_last",
            [&]() {
                return type_lookup(
                    environment,
                    type_last);
            });
    }

    std::cerr
        << "sink="
        << sink
        << '\n';

    return 0;
}