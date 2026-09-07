#pragma once

#include "../graph/builtin_type.hpp"
#include "../graph/type_ref.hpp"
#include "../language/aggregate_semantics.hpp"
#include "../language/enum_semantics.hpp"
#include "../parser/source_facts.hpp"
#include "../source_entity_ref.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../../string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace cw::server {

struct build_name_ref {
    std::uint32_t index = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return index != 0;
    }
};

struct source_build_name {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    string_id canonical{};
};

struct source_build_enum_value {
    build_name_ref name{};
    integral_constant value{};
    source_text_range name_range{};
};

struct source_build_enum {
    build_name_ref canonical_name{};
    bool anonymous = false;
    bool scoped = false;
    enum_definition_state definition_state =
        enum_definition_state::defined;
    std::optional<builtin_type> explicit_underlying;
    std::uint32_t value_offset = 0;
    std::uint32_t value_count = 0;
    source_text_range declaration_range{};
    source_text_range name_range{};
    source_entity_ref source_entity{};
};

struct source_build_modifier {
    derived_type_kind kind = derived_type_kind::pointer;
    std::uint64_t payload = 0;
};

struct source_build_member {
    build_name_ref name{};
    std::optional<builtin_type> builtin;

    // Compatibility spelling for non-Parser producers only.
    build_name_ref user_type_name{};

    std::uint32_t modifier_offset = 0;
    std::uint32_t modifier_count = 0;

    // Production relation after Parser resolution.
    source_entity_ref user_type_entity{};
};

struct source_build_aggregate {
    build_name_ref canonical_name{};
    aggregate_definition_state definition_state =
        aggregate_definition_state::declared;
    std::uint32_t member_offset = 0;
    std::uint32_t member_count = 0;
    source_text_range declaration_range{};
    source_text_range name_range{};
    source_entity_ref source_entity{};
};

// Owns one Source's transient Builder contribution. Resolved member relations
// remain source_entity_ref and are never rebound by text in production.
class source_build_entry final {
public:
    [[nodiscard]] status store_name(
        std::string_view value,
        build_name_ref& result) noexcept;

    [[nodiscard]] status resolve_name(
        build_name_ref reference,
        std::string_view& output) const noexcept;

    [[nodiscard]] status bind_name(
        build_name_ref reference,
        string_id canonical) noexcept;

    [[nodiscard]] string_id bound_name(
        build_name_ref reference) const noexcept;

    [[nodiscard]] std::size_t name_count() const noexcept {
        return name_entries.size();
    }

    [[nodiscard]] std::size_t name_bytes_size() const noexcept {
        return names.size();
    }

    void reset() noexcept;

    source_id source{};
    std::vector<source_build_enum_value> enum_values;
    std::vector<source_build_enum> enums;
    std::vector<source_build_modifier> modifiers;
    std::vector<source_build_member> members;
    std::vector<source_build_aggregate> aggregates;

private:
    std::vector<char> names;
    std::vector<source_build_name> name_entries;
};

} // namespace cw::server