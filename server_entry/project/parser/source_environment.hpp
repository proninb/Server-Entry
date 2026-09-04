#pragma once

#include "../graph/builtin_type.hpp"
#include "../../status.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cw::server {

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

// Owns the immutable Parser-visible interface exported by one Source.
// Local bindings are stored once; imported interfaces are referenced recursively,
// which preserves transitive include visibility without duplicating declarations.
class source_environment_storage final {
public:
    source_environment_storage() = default;
    source_environment_storage(const source_environment_storage&) = delete;
    source_environment_storage& operator=(const source_environment_storage&) = delete;
    source_environment_storage(source_environment_storage&&) = delete;
    source_environment_storage& operator=(source_environment_storage&&) = delete;

    [[nodiscard]] status initialize(
        std::span<const source_constant_binding> constants,
        std::span<const source_type_binding> types,
        std::span<const source_environment_storage* const> imports = {}) noexcept;

    [[nodiscard]] static constexpr std::size_t lookup_key_size() noexcept {
        return sizeof(key);
    }

private:
    friend class source_environment;

    struct key {
        std::string_view scope_name;
        std::string_view name;
        friend bool operator==(const key&, const key&) noexcept = default;
    };

    struct key_hash {
        [[nodiscard]] std::size_t operator()(const key& value) const noexcept;
    };

    [[nodiscard]] bool find_constant_recursive(
        std::string_view scope,
        std::string_view name,
        integral_constant& output) const noexcept;

    [[nodiscard]] bool find_type_recursive(
        std::string_view scope,
        std::string_view name,
        std::string_view& output) const noexcept;

    std::vector<char> spellings;
    std::unordered_map<key, integral_constant, key_hash> constants;
    std::unordered_map<key, std::string_view, key_hash> types;
    std::vector<const source_environment_storage*> imports;
    bool initialized = false;
};

// One textual include visibility event. The imported interface becomes visible
// only to tokens whose original Source byte offset is >= visible_from.
struct source_environment_import {
    std::uint32_t visible_from = 0;
    const source_environment_storage* storage = nullptr;
};

// Provides non-owning name lookup for one Parser invocation.
// Positional imports preserve the source-order semantics of #include.
class source_environment final {
public:
    source_environment() = default;

    explicit source_environment(
        const source_environment_storage& storage) noexcept
        : single(&storage) {}

    // Compatibility view: every dependency is visible for the whole Source.
    explicit source_environment(
        std::span<const source_environment_storage* const> dependencies) noexcept
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
        std::string_view& output) const noexcept;

    [[nodiscard]] status find_type_exact(
        std::string_view scope,
        std::string_view name,
        std::uint32_t source_offset,
        std::string_view& output) const noexcept;

private:
    const source_environment_storage* single = nullptr;
    std::span<const source_environment_storage* const> dependencies;
    std::span<const source_environment_import> positional_imports;
};

} // namespace cw::server
