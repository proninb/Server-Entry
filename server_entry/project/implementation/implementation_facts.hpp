#pragma once

#include "../graph/type_ref.hpp"
#include "../parser/source_facts.hpp"
#include "../../member_index.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cw::server {

struct implementation_object_fact {
    source_name_ref name{};
    TypeRef type{};

    std::uint32_t dimension_offset = 0;
    std::uint32_t dimension_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
    source_text_range type_range{};
};

enum class implementation_path_step_kind : std::uint8_t {
    member,
    index
};

struct implementation_path_step {
    implementation_path_step_kind kind =
        implementation_path_step_kind::member;
    member_index member{};
    std::uint64_t index = 0;
};

struct implementation_path_fact {
    std::uint32_t object_index = 0;
    std::uint32_t step_offset = 0;
    std::uint32_t step_count = 0;
    std::uint32_t remaining_object_dimensions = 0;
    TypeRef type{};
    source_text_range range{};
};

enum class implementation_literal_kind : std::uint8_t {
    integer,
    character,
    string
};

struct implementation_literal_fact {
    implementation_literal_kind kind =
        implementation_literal_kind::integer;
    std::uint64_t bits = 0;
    std::uint32_t byte_offset = 0;
    std::uint32_t byte_count = 0;
    source_text_range range{};
};

enum class implementation_operation_kind : std::uint8_t {
    value_literal,
    value_copy,
    binding
};

struct implementation_operation_fact {
    implementation_operation_kind kind =
        implementation_operation_kind::value_literal;

    std::uint32_t target_path = 0;
    std::uint32_t source_path = 0;
    std::uint32_t literal = 0;
};

// Immutable semantic product of one successfully parsed implementation Source.
// Source-local object/path coordinates are dense positions, not identity domains.
class implementation_facts_storage final {
public:
    [[nodiscard]] status resolve_name(
        source_name_ref reference,
        std::string_view& output) const noexcept;

    void reset() noexcept;

    source_id source{};
    std::vector<implementation_object_fact> objects;
    std::vector<std::uint64_t> object_dimensions;
    std::vector<implementation_path_fact> paths;
    std::vector<implementation_path_step> path_steps;
    std::vector<implementation_literal_fact> literals;
    std::vector<char> literal_bytes;
    std::vector<implementation_operation_fact> operations;

private:
    friend class implementation_context;

    std::uint32_t stored_name_count = 0;
    std::vector<char> names;
};

} // namespace cw::server
