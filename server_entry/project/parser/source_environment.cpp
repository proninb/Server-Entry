#include "source_environment.hpp"

#include <limits>
#include <stdexcept>

namespace cw::server {

std::size_t source_environment_storage::key_hash::operator()(
    const key& value) const noexcept {

    const auto scope_hash =
        std::hash<std::string_view>{}(value.scope_name);
    const auto name_hash =
        std::hash<std::string_view>{}(value.name);

    return scope_hash ^
        (name_hash + 0x9e3779b97f4a7c15ULL +
         (scope_hash << 6) + (scope_hash >> 2));
}

status source_environment_storage::initialize(
    std::span<const source_constant_binding> constant_bindings,
    std::span<const source_type_binding> type_bindings,
    std::span<const source_environment_storage* const> imported) noexcept {

    if (initialized) {
        return {status_code::configuration_failed};
    }

    const auto reset_candidate = [&]() noexcept {
        constants.clear();
        types.clear();
        imports.clear();
        spellings.clear();
    };

    const auto reject = [&]() noexcept {
        reset_candidate();
        return status{status_code::configuration_failed};
    };

    try {
        std::size_t spelling_bytes = 0;

        const auto count_spelling = [&](std::string_view value) {
            if (value.size() >
                (std::numeric_limits<std::size_t>::max)() - spelling_bytes) {
                throw std::length_error{"source environment spelling arena"};
            }
            spelling_bytes += value.size();
        };

        for (const auto& binding : constant_bindings) {
            count_spelling(binding.scope_name);
            count_spelling(binding.name);
        }

        for (const auto& binding : type_bindings) {
            count_spelling(binding.scope_name);
            count_spelling(binding.name);
            count_spelling(binding.canonical_name);
        }

        spellings.reserve(spelling_bytes);

        const auto store_spelling = [&](std::string_view value) {
            const auto offset = spellings.size();
            spellings.insert(spellings.end(), value.begin(), value.end());
            return std::string_view{
                spellings.data() + offset,
                value.size()
            };
        };

        constants.reserve(constant_bindings.size());
        types.reserve(type_bindings.size());
        imports.reserve(imported.size());

        for (const auto* item : imported) {
            if (!item || !item->initialized) {
                return reject();
            }
            imports.push_back(item);
        }

        for (const auto& binding : constant_bindings) {
            if (binding.name.empty() || !is_integral(binding.value.type)) {
                return reject();
            }

            const auto scope = store_spelling(binding.scope_name);
            const auto name = store_spelling(binding.name);
            const key lookup{scope, name};

            const auto found = constants.find(lookup);
            if (found != constants.end()) {
                if (found->second.type != binding.value.type ||
                    found->second.bits != binding.value.bits) {
                    return reject();
                }
                continue;
            }

            constants.emplace(lookup, binding.value);
        }

        for (const auto& binding : type_bindings) {
            if (binding.name.empty() || binding.canonical_name.empty()) {
                return reject();
            }

            const auto scope = store_spelling(binding.scope_name);
            const auto name = store_spelling(binding.name);
            const auto canonical = store_spelling(binding.canonical_name);
            const key lookup{scope, name};

            const auto found = types.find(lookup);
            if (found != types.end()) {
                // Multiple declarations of the same source Entity coalesce.
                if (found->second != canonical) {
                    return reject();
                }
                continue;
            }

            types.emplace(lookup, canonical);
        }

        initialized = true;
        return {};
    }
    catch (...) {
        reset_candidate();
        return {status_code::initialization_failed};
    }
}

bool source_environment_storage::find_constant_recursive(
    std::string_view scope,
    std::string_view name,
    integral_constant& output) const noexcept {

    const auto local = constants.find({scope, name});
    if (local != constants.end()) {
        output = local->second;
        return true;
    }

    for (const auto* imported : imports) {
        if (imported &&
            imported->find_constant_recursive(scope, name, output)) {
            return true;
        }
    }

    return false;
}

bool source_environment_storage::find_type_recursive(
    std::string_view scope,
    std::string_view name,
    std::string_view& output) const noexcept {

    const auto local = types.find({scope, name});
    if (local != types.end()) {
        output = local->second;
        return true;
    }

    for (const auto* imported : imports) {
        if (imported &&
            imported->find_type_recursive(scope, name, output)) {
            return true;
        }
    }

    return false;
}

status source_environment::find_constant_exact(
    std::string_view scope,
    std::string_view name,
    integral_constant& output) const noexcept {

    return find_constant_exact(
        scope,
        name,
        (std::numeric_limits<std::uint32_t>::max)(),
        output);
}

status source_environment::find_constant_exact(
    std::string_view scope,
    std::string_view name,
    std::uint32_t source_offset,
    integral_constant& output) const noexcept {

    output = {};

    if (name.empty()) {
        return {status_code::configuration_failed};
    }

    if (single && single->find_constant_recursive(scope, name, output)) {
        return {};
    }

    for (const auto* dependency : dependencies) {
        if (dependency &&
            dependency->find_constant_recursive(scope, name, output)) {
            return {};
        }
    }

    for (const auto& imported : positional_imports) {
        if (imported.visible_from <= source_offset &&
            imported.storage &&
            imported.storage->find_constant_recursive(scope, name, output)) {
            return {};
        }
    }

    return {status_code::configuration_failed};
}

status source_environment::find_type_exact(
    std::string_view scope,
    std::string_view name,
    std::string_view& output) const noexcept {

    return find_type_exact(
        scope,
        name,
        (std::numeric_limits<std::uint32_t>::max)(),
        output);
}

status source_environment::find_type_exact(
    std::string_view scope,
    std::string_view name,
    std::uint32_t source_offset,
    std::string_view& output) const noexcept {

    output = {};

    if (name.empty()) {
        return {status_code::configuration_failed};
    }

    if (single && single->find_type_recursive(scope, name, output)) {
        return {};
    }

    for (const auto* dependency : dependencies) {
        if (dependency &&
            dependency->find_type_recursive(scope, name, output)) {
            return {};
        }
    }

    for (const auto& imported : positional_imports) {
        if (imported.visible_from <= source_offset &&
            imported.storage &&
            imported.storage->find_type_recursive(scope, name, output)) {
            return {};
        }
    }

    return {status_code::configuration_failed};
}

} // namespace cw::server
