#pragma once

#include "../project_configuration.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>

namespace cw::server {

// Identifies an intrinsic source-language builtin type.
// Builtins use language/ABI intrinsic codes and do not receive allocated stable_id
// values. Declaration order is also used by the constexpr category predicates:
// integral types must remain contiguous through character_32 and floating types
// must remain contiguous from floating through long_double_floating.
enum class builtin_type : std::uint8_t {
    boolean,
    character,
    signed_character,
    unsigned_character,
    short_integer,
    unsigned_short_integer,
    integer,
    unsigned_integer,
    long_integer,
    unsigned_long_integer,
    long_long_integer,
    unsigned_long_long_integer,
    wide_character,
    character_8,
    character_16,
    character_32,
    floating,
    double_floating,
    long_double_floating,
    void_type
};

// Stores one Parser-interpreted integral constant.
// bits contains the raw value representation; type determines signedness and the
// interpretation required by later language/ABI semantic operations.
struct integral_constant {
    builtin_type type = builtin_type::integer;
    std::uint64_t bits = 0;
};

[[nodiscard]] constexpr bool is_integral(builtin_type type) noexcept {
    return type <= builtin_type::character_32;
}

[[nodiscard]] constexpr bool is_floating(builtin_type type) noexcept {
    return
        type >= builtin_type::floating &&
        type <= builtin_type::long_double_floating;
}

// Reports language-level signedness for builtins whose signedness is intrinsic.
// Plain character is ABI-dependent and is handled by builtin_is_signed().
[[nodiscard]] constexpr bool is_signed(builtin_type type) noexcept {
    switch (type) {
    case builtin_type::signed_character:
    case builtin_type::short_integer:
    case builtin_type::integer:
    case builtin_type::long_integer:
    case builtin_type::long_long_integer:
        return true;

    default:
        return false;
    }
}

// Returns the storage width defined by the configured target ABI.
// windows_x64 follows LLP64; posix_x64 follows the project LP64 contract.
// A result of zero denotes an unsupported/non-sized builtin in this API.
[[nodiscard]] std::uint8_t builtin_bit_width(
    builtin_type type,
    const abi_configuration& abi) noexcept;

// Resolves target-ABI signedness, including ABI-dependent character types.
[[nodiscard]] bool builtin_is_signed(
    builtin_type type,
    const abi_configuration& abi) noexcept;

// Selects the target-ABI underlying type for one unscoped enum value set.
[[nodiscard]] status select_unscoped_enum_underlying(
    std::span<const integral_constant> values,
    const abi_configuration& abi,
    builtin_type& output) noexcept;

// Applies the same enum-underlying selection to an arbitrary range whose
// elements can be projected to integral_constant without materializing a copy.
template<class Range, class Projection>
[[nodiscard]] status select_unscoped_enum_underlying_projected(
    const Range& values,
    const abi_configuration& abi,
    builtin_type& output,
    Projection projection) noexcept {

    if (!is_supported_abi_target(abi.target)) {
        return {status_code::configuration_failed};
    }

    std::int64_t minimum = 0;
    std::uint64_t maximum = 0;
    bool has_negative = false;

    for (const auto& item : values) {
        const integral_constant value =
            std::invoke(projection, item);

        if (!is_integral(value.type)) {
            return {status_code::configuration_failed};
        }

        const auto width =
            builtin_bit_width(value.type, abi);

        if (width == 0 || width > 64) {
            return {status_code::configuration_failed};
        }

        if (builtin_is_signed(value.type, abi)) {
            const auto signed_value =
                width == 64
                    ? static_cast<std::int64_t>(value.bits)
                    : static_cast<std::int64_t>(
                          value.bits << (64 - width)) >>
                          (64 - width);

            if (signed_value < 0) {
                has_negative = true;

                if (signed_value < minimum) {
                    minimum = signed_value;
                }
            }
            else if (static_cast<std::uint64_t>(signed_value) > maximum) {
                maximum =
                    static_cast<std::uint64_t>(signed_value);
            }
        }
        else if (value.bits > maximum) {
            maximum = value.bits;
        }
    }

    if (minimum >=
            (std::numeric_limits<std::int32_t>::min)() &&
        maximum <= static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)())) {
        output = builtin_type::integer;
        return {};
    }

    if (!has_negative &&
        maximum <=
            (std::numeric_limits<std::uint32_t>::max)()) {
        output = builtin_type::unsigned_integer;
        return {};
    }

    switch (abi.target) {
    case abi_target::windows_x64:
        if (maximum <= static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            output = builtin_type::long_long_integer;
            return {};
        }

        if (!has_negative) {
            output = builtin_type::unsigned_long_long_integer;
            return {};
        }

        break;

    case abi_target::posix_x64:
        // The POSIX x64 target uses LP64, so long is the first 64-bit
        // candidate after int/unsigned int.
        if (maximum <= static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            output = builtin_type::long_integer;
            return {};
        }

        if (!has_negative) {
            output = builtin_type::unsigned_long_integer;
            return {};
        }

        break;
    }

    return {status_code::configuration_failed};
}

} // namespace cw::server
