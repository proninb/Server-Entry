#pragma once

#include "builtin_type.hpp"
#include "../language/enum_semantics.hpp"
#include "../../string_id.hpp"

#include <optional>
#include <span>

namespace cw::server {

// Canonical Builder-to-Graph enum value before target-ABI materialization.
struct enum_value_build {
    string_id name{};
    integral_constant value{};
};

// Canonical Builder-to-Graph enum declaration or definition.
struct enum_build_data {
    enum_definition_state definition_state =
        enum_definition_state::defined;
    bool scoped = false;
    std::optional<builtin_type> explicit_underlying;
    std::span<const enum_value_build> enumerators;
};

} // namespace cw::server
