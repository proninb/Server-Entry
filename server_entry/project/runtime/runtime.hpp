#pragma once

#include "../../status.hpp"
#include "../implementation/implementation_facts.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace cw::server {

class graph;
class project_context;

// Read-only view over one materialized Runtime storage object.
// bytes is the ABI object image used by execution; element_count is greater than
// one only for an Implementation object declared with Source-level dimensions.
struct runtime_storage_view {
    TypeRef type{};
    std::span<const std::byte> bytes;
    std::uint64_t element_count = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return !bytes.empty() && static_cast<bool>(type);
    }
};

// Owns execution-facing object storage for one Project. Runtime consumes only
// resolved Implementation facts and committed Graph ABI coordinates; it never
// performs source-language lookup or recomputes type layout.
class runtime {
public:
    runtime();
    ~runtime();

    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    runtime(runtime&&) = delete;
    runtime& operator=(runtime&&) = delete;

    [[nodiscard]] status attach(const graph& graph) noexcept;

    [[nodiscard]] status attach(
        const graph& graph,
        std::vector<implementation_facts_storage>&& implementation) noexcept;

    [[nodiscard]] std::span<const implementation_facts_storage>
        implementation_sources() const noexcept {
        return implementation_source_facts;
    }

    [[nodiscard]] status object(
        source_id source,
        std::uint32_t object_index,
        runtime_storage_view& output) const noexcept;

    [[nodiscard]] status static_object(
        std::uint32_t object_index,
        runtime_storage_view& output) const noexcept;

private:
    friend class project_context;

    // Builds all sparse replacement storage and reserves any new Source slots
    // before Source Manager publication. The method does not change published
    // execution-facing object facts or addresses.
    [[nodiscard]] status prepare_implementation_replacements(
        std::span<const implementation_facts_storage> replacements) noexcept;

    // Called only after successful prepare + Source Manager commit. Every
    // allocation required by replacement or new-Source publication is complete.
    void publish_implementation_replacements(
        std::vector<implementation_facts_storage>&& replacements) noexcept;

    struct materialized_state;

    std::unique_ptr<materialized_state> materialized;
    std::vector<implementation_facts_storage> implementation_source_facts;
    std::vector<std::uint32_t> implementation_source_index;
    std::vector<std::uint32_t> implementation_validation_marks;
    std::uint32_t implementation_validation_generation = 1;
};

} // namespace cw::server
