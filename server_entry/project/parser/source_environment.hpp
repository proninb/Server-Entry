#pragma once

#include "../graph/builtin_type.hpp"
#include "../../status.hpp"

#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cw::server
{
struct source_constant_binding
{
    std::string_view scope_name, name;
    integral_constant value;
};
struct source_type_binding
{
    std::string_view scope_name, name, canonical_name;
};

class source_environment_storage final
{
public:
    source_environment_storage() = default;
    source_environment_storage(const source_environment_storage&) = delete;
    source_environment_storage& operator=(const source_environment_storage&) = delete;
    source_environment_storage(source_environment_storage&&) = delete;
    source_environment_storage& operator=(source_environment_storage&&) = delete;

    [[nodiscard]] status initialize(std::span<const source_constant_binding>,
                                    std::span<const source_type_binding>) noexcept;

private:
    friend class source_environment;
    struct key
    {
        std::string_view scope_name, name;
        friend bool operator==(const key&, const key&) noexcept = default;
    };
    struct key_hash { std::size_t operator()(const key&) const noexcept; };
    std::vector<char> spellings_;
    std::unordered_map<key, integral_constant, key_hash> constants_;
    std::unordered_map<key, std::string_view, key_hash> types_;
    bool initialized_ = false;

public:
    [[nodiscard]] static constexpr std::size_t lookup_key_size() noexcept
    {
        return sizeof(key);
    }
};

class source_environment final
{
public:
    source_environment() = default;
    explicit source_environment(const source_environment_storage& storage) noexcept
        : single_(&storage) {}
    explicit source_environment(
        std::span<const source_environment_storage* const> dependencies) noexcept
        : dependencies_(dependencies) {}

    [[nodiscard]] status find_constant_exact(
        std::string_view, std::string_view, integral_constant&) const noexcept;
    [[nodiscard]] status find_type_exact(
        std::string_view, std::string_view, std::string_view&) const noexcept;

private:
    const source_environment_storage* single_ = nullptr;
    std::span<const source_environment_storage* const> dependencies_;
};
}
