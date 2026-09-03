#include "source_environment.hpp"

#include <limits>
#include <stdexcept>

namespace cw::server
{
std::size_t source_environment_storage::key_hash::operator()(const key& value) const noexcept
{
    const auto first = std::hash<std::string_view>{}(value.scope_name);
    const auto second = std::hash<std::string_view>{}(value.name);
    return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6) + (first >> 2));
}

status source_environment_storage::initialize(
    std::span<const source_constant_binding> constants,
    std::span<const source_type_binding> types) noexcept
{
    if (initialized_) return {status_code::configuration_failed};
    const auto reset_candidate = [&]() noexcept
    {
        constants_.clear();
        types_.clear();
        spellings_.clear();
    };
    const auto reject = [&]() noexcept
    {
        reset_candidate();
        return status{status_code::configuration_failed};
    };
    try
    {
        std::size_t bytes = 0;
        const auto add = [&](std::string_view value)
        {
            if (value.size() > (std::numeric_limits<std::size_t>::max)() - bytes)
                throw std::length_error{"source interface spelling arena"};
            bytes += value.size();
        };
        for (const auto& binding : constants) { add(binding.scope_name); add(binding.name); }
        for (const auto& binding : types)
        { add(binding.scope_name); add(binding.name); add(binding.canonical_name); }
        spellings_.reserve(bytes);
        const auto store = [&](std::string_view value)
        {
            const auto offset = spellings_.size();
            spellings_.insert(spellings_.end(), value.begin(), value.end());
            return std::string_view{spellings_.data() + offset, value.size()};
        };
        constants_.reserve(constants.size());
        types_.reserve(types.size());
        for (const auto& binding : constants)
        {
            if (binding.name.empty() ||
                (binding.value.type != builtin_type::long_long_integer &&
                 binding.value.type != builtin_type::unsigned_long_long_integer) ||
                !constants_.emplace(key{store(binding.scope_name), store(binding.name)},
                                    binding.value).second)
                return reject();
        }
        for (const auto& binding : types)
        {
            if (binding.name.empty() || binding.canonical_name.empty())
                return reject();
            const auto scope = store(binding.scope_name);
            const auto name = store(binding.name);
            const auto canonical = store(binding.canonical_name);
            if (!types_.emplace(key{scope, name}, canonical).second)
                return reject();
        }
        initialized_ = true;
        return {};
    }
    catch (...)
    {
        reset_candidate();
        return {status_code::initialization_failed};
    }
}

status source_environment::find_constant_exact(
    std::string_view scope, std::string_view name, integral_constant& output) const noexcept
{
    output = {};
    if (name.empty()) return {status_code::configuration_failed};
    const auto find = [&](const source_environment_storage* storage)
    {
        if (!storage) return false;
        const auto position = storage->constants_.find({scope, name});
        if (position == storage->constants_.end()) return false;
        output = position->second;
        return true;
    };
    if (find(single_)) return {};
    for (const auto* dependency : dependencies_) if (find(dependency)) return {};
    return {status_code::configuration_failed};
}

status source_environment::find_type_exact(
    std::string_view scope, std::string_view name, std::string_view& output) const noexcept
{
    output = {};
    if (name.empty()) return {status_code::configuration_failed};
    const auto find = [&](const source_environment_storage* storage)
    {
        if (!storage) return false;
        const auto position = storage->types_.find({scope, name});
        if (position == storage->types_.end()) return false;
        output = position->second;
        return true;
    };
    if (find(single_)) return {};
    for (const auto* dependency : dependencies_) if (find(dependency)) return {};
    return {status_code::configuration_failed};
}
}
