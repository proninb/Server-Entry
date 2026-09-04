#pragma once

#include "diagnostic_descriptor.hpp"

#include <array>
#include <span>

namespace cw::server {

// Provides read-only lookup over the immutable diagnostic descriptor catalog.
// The view does not own descriptor storage and is intended for lightweight
// resolution of diagnostic_id to its catalog definition.
class diagnostic_registry_view {
public:
    constexpr explicit diagnostic_registry_view(
        std::span<const diagnostic_descriptor> descriptors) noexcept
        : descriptors(descriptors) {}

    [[nodiscard]] constexpr const diagnostic_descriptor* find(diagnostic_id id) const noexcept {
        for (const auto& descriptor : descriptors) {
            if (descriptor.id == id) {
                return &descriptor;
            }
        }

        return nullptr;
    }

private:
    std::span<const diagnostic_descriptor> descriptors;
};

// Single compile-time registry of all diagnostics exposed by Server subsystems.
// Keeping the catalog centralized allows identity and naming invariants to be
// validated before the program is built.
inline constexpr std::array diagnostic_descriptors{
    diagnostics::server_initialization_failed,
    diagnostics::server_invalid_json,
    diagnostics::server_invalid_configuration,
    diagnostics::server_unsupported_configuration_version,
    diagnostics::server_configuration_read_failed,
    diagnostics::project_initialization_failed,
    diagnostics::project_invalid_json,
    diagnostics::project_invalid_configuration,
    diagnostics::project_unsupported_configuration_version,
    diagnostics::project_configuration_read_failed,
    diagnostics::project_composition_cycle,
    diagnostics::project_composition_failed,
    diagnostics::source_initialization_failed,
    diagnostics::source_include_cycle,
    diagnostics::source_include_not_found,
    diagnostics::source_unsupported_directive,
    diagnostics::source_include_inside_namespace,
    diagnostics::source_acquisition_failed,
    diagnostics::source_checkpoint_save_failed,
    diagnostics::parser_invalid_source,
    diagnostics::parser_invalid_enum_forward_declaration,
    diagnostics::parser_anonymous_opaque_enum,
    diagnostics::parser_invalid_enum_underlying,
    diagnostics::parser_expected_enumerator_identifier,
    diagnostics::parser_expected_semicolon,
    diagnostics::parser_invalid_enumerator_expression,
    diagnostics::parser_duplicate_enumerator,
    diagnostics::parser_initialization_failed,
    diagnostics::builder_duplicate_source_replacement,
    diagnostics::builder_invalid_source_fact,
    diagnostics::builder_semantic_failure,
    diagnostics::canonicalization_failed,
    diagnostics::construction_initialization_failed,
    diagnostics::runtime_attach_failed,
    diagnostics::shm_initialization_failed,
};

inline constexpr diagnostic_registry_view diagnostic_registry{diagnostic_descriptors};

consteval bool diagnostic_ids_unique() {
    for (std::size_t left = 0; left < diagnostic_descriptors.size(); ++left) {
        for (std::size_t right = left + 1; right < diagnostic_descriptors.size(); ++right) {
            if (diagnostic_descriptors[left].id == diagnostic_descriptors[right].id) {
                return false;
            }
        }
    }

    return true;
}

consteval bool diagnostic_names_unique() {
    for (std::size_t left = 0; left < diagnostic_descriptors.size(); ++left) {
        for (std::size_t right = left + 1; right < diagnostic_descriptors.size(); ++right) {
            if (diagnostic_descriptors[left].name == diagnostic_descriptors[right].name) {
                return false;
            }
        }
    }

    return true;
}

consteval bool diagnostic_descriptors_valid() {
    for (const auto& descriptor : diagnostic_descriptors) {
        if (!descriptor.id || descriptor.domain == diagnostic_domain::unknown ||
            descriptor.name.empty() || descriptor.message.empty()) {
            return false;
        }
    }

    return true;
}

static_assert(diagnostic_ids_unique());
static_assert(diagnostic_names_unique());
static_assert(diagnostic_descriptors_valid());

} // namespace cw::server
