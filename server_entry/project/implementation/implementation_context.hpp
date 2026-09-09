#pragma once

#include "implementation_facts.hpp"
#include "implementation_token.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cw::server {

// Reusable Source-local implementation construction workspace. It owns
// transient object-name lookup and diagnostics, then transfers only resolved
// semantic facts into implementation_facts_storage.
class implementation_context final {
public:
    [[nodiscard]] status store_name(
        std::string_view value,
        source_name_ref& result) noexcept;

    [[nodiscard]] status resolve_name(
        source_name_ref reference,
        std::string_view& output) const noexcept;

    [[nodiscard]] status declare_object(
        std::string_view spelling,
        const implementation_object_fact& fact,
        std::uint32_t& object) noexcept;

    [[nodiscard]] status find_object(
        std::string_view spelling,
        std::uint32_t& object) const noexcept;

    [[nodiscard]] status release_facts(
        source_id source,
        implementation_facts_storage& output) noexcept;

    void reset() noexcept;

    std::vector<implementation_token> tokens;
    std::vector<implementation_object_fact> objects;
    std::vector<std::uint64_t> object_dimensions;
    std::vector<implementation_path_fact> paths;
    std::vector<implementation_path_step> path_steps;
    std::vector<implementation_literal_fact> literals;
    std::vector<char> literal_bytes;
    std::vector<implementation_operation_fact> operations;
    diagnostic_buffer diagnostics;

private:
    [[nodiscard]] status ensure_object_index(
        std::size_t required) noexcept;

    std::uint32_t stored_name_count = 0;
    std::vector<char> names;
    std::vector<std::uint32_t> object_index;
};

} // namespace cw::server
