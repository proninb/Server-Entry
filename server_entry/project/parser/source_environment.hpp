#pragma once

#include "source_facts.hpp"
#include "../graph/builtin_type.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

class source_context;

struct source_constant_binding {
    std::string_view scope_name;
    std::string_view name;
    integral_constant value;
};

struct source_type_binding {
    std::string_view scope_name;
    std::string_view name;
    std::string_view canonical_name;
};

// Immutable semantic projection exported only when another Source depends on it.
// Semantic records remain dense; compact exact-lookup sidecars are built once
// with the interface and reused by every dependent Parser.
class source_interface_storage final {
public:
    source_interface_storage() = default;
    source_interface_storage(const source_interface_storage&) = delete;
    source_interface_storage& operator=(const source_interface_storage&) = delete;
    source_interface_storage(source_interface_storage&&) = delete;
    source_interface_storage& operator=(source_interface_storage&&) = delete;

    [[nodiscard]] status initialize(
        source_id source,
        const source_context& context,
        std::span<const source_interface_storage* const> imports = {}) noexcept;

    // Compatibility seam for focused Parser tests/non-frontend callers.
    [[nodiscard]] status initialize(
        std::span<const source_constant_binding> constants,
        std::span<const source_type_binding> types,
        std::span<const source_interface_storage* const> imports = {}) noexcept;

    [[nodiscard]] static constexpr std::size_t lookup_key_size() noexcept {
        return sizeof(name_range);
    }

private:
    friend class source_environment;

    struct name_range {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };

    struct constant_record {
        name_range lookup_name{};
        integral_constant value{};
    };

    struct type_record {
        name_range lookup_name{};
        name_range canonical_name{};
        source_entity_ref entity{};
    };

    [[nodiscard]] std::string_view spelling(
        name_range value) const noexcept;

    [[nodiscard]] status build_lookup_indexes() noexcept;

    [[nodiscard]] bool find_constant_local(
        std::string_view scope,
        std::string_view name,
        integral_constant& output) const noexcept;

    [[nodiscard]] bool find_type_local(
        std::string_view scope,
        std::string_view name,
        source_entity_ref& entity,
        std::string_view& canonical) const noexcept;

    [[nodiscard]] bool find_constant_recursive(
        std::string_view scope,
        std::string_view name,
        integral_constant& output) const noexcept;

    [[nodiscard]] bool find_type_recursive(
        std::string_view scope,
        std::string_view name,
        source_entity_ref& entity,
        std::string_view& canonical) const noexcept;

    std::vector<char> spellings;
    std::vector<constant_record> constants;
    std::vector<type_record> types;

    // One-based record positions. These sidecars contain no semantic state.
    std::vector<std::uint32_t> constant_index;
    std::vector<std::uint32_t> type_index;

    std::vector<const source_interface_storage*> imports;
    bool initialized = false;
};

// Compatibility name for existing frontend/cache APIs.
using source_environment_storage = source_interface_storage;

struct source_environment_import {
    std::uint32_t visible_from = 0;
    const source_interface_storage* storage = nullptr;
};

// Non-owning Parser resolver over visible Source interfaces.
// Lookup happens only for an actual unresolved source-language reference.
class source_environment final {
public:
    source_environment() = default;

    explicit source_environment(
        const source_interface_storage& storage) noexcept
        : single(&storage) {}

    explicit source_environment(
        std::span<const source_interface_storage* const> dependencies) noexcept
        : dependencies(dependencies) {}

    explicit source_environment(
        std::span<const source_environment_import> imports) noexcept
        : positional_imports(imports) {}

    [[nodiscard]] status find_constant_exact(
        std::string_view scope,
        std::string_view name,
        integral_constant& output) const noexcept;

    [[nodiscard]] status find_constant_exact(
        std::string_view scope,
        std::string_view name,
        std::uint32_t source_offset,
        integral_constant& output) const noexcept;

    [[nodiscard]] status find_type_exact(
        std::string_view scope,
        std::string_view name,
        std::string_view& canonical) const noexcept;

    [[nodiscard]] status find_type_exact(
        std::string_view scope,
        std::string_view name,
        std::uint32_t source_offset,
        std::string_view& canonical) const noexcept;

    [[nodiscard]] status find_type_exact(
        std::string_view scope,
        std::string_view name,
        std::uint32_t source_offset,
        source_entity_ref& entity,
        std::string_view& canonical) const noexcept;

private:
    const source_interface_storage* single = nullptr;
    std::span<const source_interface_storage* const> dependencies;
    std::span<const source_environment_import> positional_imports;
};

} // namespace cw::server