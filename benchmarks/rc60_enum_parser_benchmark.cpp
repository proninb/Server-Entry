#include "../server_entry/project/parser/parser.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
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
constexpr int measured_runs = 11;

std::string make_enum(std::size_t count) {
    std::string text;
    text.reserve(32 + count * 18);
    text += "enum Large : int {";

    for (std::size_t index = 0;
         index != count;
         ++index) {

        text += 'E';
        text += std::to_string(index);
        text += '=';
        text += std::to_string(index);
        text += ',';
    }

    text += "};";
    return text;
}

bool parse_once(
    std::string_view text,
    std::size_t expected,
    double& elapsed_ms) noexcept {

    source_context context;
    source_environment environment;

    const auto begin =
        clock_type::now();

    const auto result =
        parse_source(
            source_view{
                source_id{1},
                text
            },
            environment,
            operation_id{1},
            context);

    const auto end =
        clock_type::now();

    elapsed_ms =
        std::chrono::duration<double, std::milli>(
            end - begin).count();

    if (!result.ok() ||
        !context.diagnostics.empty() ||
        context.enums.size() != 1 ||
        context.enum_values.size() != expected) {
        return false;
    }

    if (expected != 0) {
        if (context.enum_values.front().value.bits != 0 ||
            context.enum_values.back().value.bits !=
                static_cast<std::uint64_t>(
                    expected - 1)) {
            return false;
        }
    }

    return true;
}

} // namespace

int main() {
    std::cout
        << std::fixed
        << std::setprecision(6)
        << "n,run,ms,values,diagnostics\n";

    for (const auto count : sizes) {
        const auto text =
            make_enum(count);

        for (int run = 0;
             run != warmup_runs;
             ++run) {

            double elapsed_ms = 0.0;

            if (!parse_once(
                    text,
                    count,
                    elapsed_ms)) {
                std::cerr
                    << "warmup parse failed at n="
                    << count
                    << '\n';

                return 1;
            }
        }

        for (int run = 0;
             run != measured_runs;
             ++run) {

            double elapsed_ms = 0.0;

            if (!parse_once(
                    text,
                    count,
                    elapsed_ms)) {
                std::cerr
                    << "measured parse failed at n="
                    << count
                    << ", run="
                    << run
                    << '\n';

                return 1;
            }

            std::cout
                << count
                << ','
                << run
                << ','
                << elapsed_ms
                << ','
                << count
                << ','
                << 0
                << '\n';
        }
    }

    return 0;
}