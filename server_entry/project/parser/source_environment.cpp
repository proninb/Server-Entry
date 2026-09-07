#include "source_environment.hpp"

#include "source_context.hpp"

#include <limits>
#include <stdexcept>

namespace cw::server {
namespace {

bool qualified_equal(
    std::string_view stored,
    std::string_view scope,
    std::string_view name) noexcept {

    if (name.empty()) {
        return false;
    }

    if (scope.empty()) {
        return stored == name;
    }

    if (stored.size() !=
        scope.size() + 2 + name.size()) {
        return false;
    }

    return
        stored.substr(0, scope.size()) == scope &&
        stored[scope.size()] == ':' &&
        stored[scope.size() + 1] == ':' &&
        stored.substr(scope.size() + 2) == name;
}

std::uint64_t qualified_size(
    std::string_view scope,
    std::string_view name) noexcept {

    return
        static_cast<std::uint64_t>(scope.size()) +
        (scope.empty() ? 0u : 2u) +
        static_cast<std::uint64_t>(name.size());
}

} // namespace

std::string_view source_interface_storage::spelling(
    name_range value) const noexcept {

    const auto end =
        std::uint64_t{value.offset} +
        value.length;

    if (value.length == 0 ||
        end > spellings.size()) {
        return {};
    }

    return {
        spellings.data() + value.offset,
        value.length
    };
}

status source_interface_storage::initialize(
    source_id source,
    const source_context& context,
    std::span<const source_interface_storage* const> imported) noexcept {

    if (initialized || !source) {
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
        const auto maximum =
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)());

        std::uint64_t spelling_bytes = 0;

        const auto count_exact =
            [&](std::string_view value) {
                if (value.empty() ||
                    value.size() > maximum - spelling_bytes) {
                    throw std::length_error{
                        "SourceInterface spelling arena"
                    };
                }

                spelling_bytes += value.size();
            };

        const auto count_qualified =
            [&](std::string_view scope,
                std::string_view name) {
                const auto count =
                    qualified_size(scope, name);

                if (name.empty() ||
                    count > maximum - spelling_bytes) {
                    throw std::length_error{
                        "SourceInterface qualified spelling"
                    };
                }

                spelling_bytes += count;
            };

        for (const auto* item : imported) {
            if (!item || !item->initialized) {
                return reject();
            }
        }

        for (const auto& declaration :
             context.type_declarations) {
            if (!declaration.entity ||
                declaration.entity.source != source) {
                return reject();
            }

            std::string_view canonical;

            if (!context.resolve_name(
                    declaration.canonical_name,
                    canonical).ok()) {
                return reject();
            }

            count_exact(canonical);
        }

        for (const auto& fact : context.enums) {
            std::string_view scope;
            std::string_view canonical;

            if (fact.scope_name &&
                !context.resolve_name(
                    fact.scope_name,
                    scope).ok()) {
                return reject();
            }

            if (!fact.anonymous &&
                !context.resolve_name(
                    fact.canonical_name,
                    canonical).ok()) {
                return reject();
            }

            const auto value_scope =
                fact.scoped
                    ? canonical
                    : scope;

            const auto values =
                context.enumerators(fact);

            if (values.size() != fact.enumerator_count) {
                return reject();
            }

            for (const auto& value : values) {
                std::string_view name;

                if (!context.resolve_name(
                        value.name,
                        name).ok()) {
                    return reject();
                }

                count_qualified(value_scope, name);
            }
        }

        spellings.reserve(
            static_cast<std::size_t>(spelling_bytes));

        types.reserve(context.type_declarations.size());
        constants.reserve(context.enum_values.size());

        imports.assign(
            imported.begin(),
            imported.end());

        const auto store_exact =
            [&](std::string_view value) {
                const name_range result{
                    static_cast<std::uint32_t>(spellings.size()),
                    static_cast<std::uint32_t>(value.size())
                };

                spellings.insert(
                    spellings.end(),
                    value.begin(),
                    value.end());

                return result;
            };

        const auto store_qualified =
            [&](std::string_view scope,
                std::string_view name) {
                const auto begin =
                    static_cast<std::uint32_t>(spellings.size());

                if (!scope.empty()) {
                    spellings.insert(
                        spellings.end(),
                        scope.begin(),
                        scope.end());

                    spellings.push_back(':');
                    spellings.push_back(':');
                }

                spellings.insert(
                    spellings.end(),
                    name.begin(),
                    name.end());

                return name_range{
                    begin,
                    static_cast<std::uint32_t>(
                        spellings.size() - begin)
                };
            };

        for (const auto& declaration :
             context.type_declarations) {
            std::string_view canonical;

            if (!context.resolve_name(
                    declaration.canonical_name,
                    canonical).ok()) {
                return reject();
            }

            const auto stored =
                store_exact(canonical);

            types.push_back({
                stored,
                stored,
                declaration.entity
            });
        }

        for (const auto& fact : context.enums) {
            std::string_view scope;
            std::string_view canonical;

            if (fact.scope_name &&
                !context.resolve_name(
                    fact.scope_name,
                    scope).ok()) {
                return reject();
            }

            if (!fact.anonymous &&
                !context.resolve_name(
                    fact.canonical_name,
                    canonical).ok()) {
                return reject();
            }

            const auto value_scope =
                fact.scoped
                    ? canonical
                    : scope;

            for (const auto& value :
                 context.enumerators(fact)) {
                std::string_view name;

                if (!context.resolve_name(
                        value.name,
                        name).ok() ||
                    !is_integral(value.value.type)) {
                    return reject();
                }

                constants.push_back({
                    store_qualified(value_scope, name),
                    value.value
                });
            }
        }

        initialized = true;
        return {};
    }
    catch (...) {
        reset_candidate();
        return {status_code::initialization_failed};
    }
}

status source_interface_storage::initialize(
    std::span<const source_constant_binding> constant_bindings,
    std::span<const source_type_binding> type_bindings,
    std::span<const source_interface_storage* const> imported) noexcept {

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
        const auto maximum =
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)());

        std::uint64_t spelling_bytes = 0;

        for (const auto* item : imported) {
            if (!item || !item->initialized) {
                return reject();
            }
        }

        for (const auto& binding : constant_bindings) {
            if (binding.name.empty() ||
                !is_integral(binding.value.type)) {
                return reject();
            }

            const auto count =
                qualified_size(
                    binding.scope_name,
                    binding.name);

            if (count > maximum - spelling_bytes) {
                throw std::length_error{
                    "SourceInterface constant spelling"
                };
            }

            spelling_bytes += count;
        }

        for (const auto& binding : type_bindings) {
            if (binding.name.empty() ||
                binding.canonical_name.empty()) {
                return reject();
            }

            const auto lookup_count =
                qualified_size(
                    binding.scope_name,
                    binding.name);

            const auto canonical_count =
                static_cast<std::uint64_t>(
                    binding.canonical_name.size());

            if (lookup_count > maximum - spelling_bytes ||
                canonical_count >
                    maximum - spelling_bytes - lookup_count) {
                throw std::length_error{
                    "SourceInterface type spelling"
                };
            }

            spelling_bytes +=
                lookup_count +
                canonical_count;
        }

        spellings.reserve(
            static_cast<std::size_t>(spelling_bytes));

        constants.reserve(constant_bindings.size());
        types.reserve(type_bindings.size());

        imports.assign(
            imported.begin(),
            imported.end());

        const auto store_exact =
            [&](std::string_view value) {
                const name_range result{
                    static_cast<std::uint32_t>(spellings.size()),
                    static_cast<std::uint32_t>(value.size())
                };

                spellings.insert(
                    spellings.end(),
                    value.begin(),
                    value.end());

                return result;
            };

        const auto store_qualified =
            [&](std::string_view scope,
                std::string_view name) {
                const auto begin =
                    static_cast<std::uint32_t>(spellings.size());

                if (!scope.empty()) {
                    spellings.insert(
                        spellings.end(),
                        scope.begin(),
                        scope.end());

                    spellings.push_back(':');
                    spellings.push_back(':');
                }

                spellings.insert(
                    spellings.end(),
                    name.begin(),
                    name.end());

                return name_range{
                    begin,
                    static_cast<std::uint32_t>(
                        spellings.size() - begin)
                };
            };

        for (const auto& binding : constant_bindings) {
            bool duplicate = false;

            for (const auto& existing : constants) {
                if (!qualified_equal(
                        spelling(existing.lookup_name),
                        binding.scope_name,
                        binding.name)) {
                    continue;
                }

                if (existing.value.type != binding.value.type ||
                    existing.value.bits != binding.value.bits) {
                    return reject();
                }

                duplicate = true;
                break;
            }

            if (!duplicate) {
                constants.push_back({
                    store_qualified(
                        binding.scope_name,
                        binding.name),
                    binding.value
                });
            }
        }

        std::uint32_t declaration = 0;

        for (const auto& binding : type_bindings) {
            bool duplicate = false;

            for (const auto& existing : types) {
                if (!qualified_equal(
                        spelling(existing.lookup_name),
                        binding.scope_name,
                        binding.name)) {
                    continue;
                }

                if (spelling(existing.canonical_name) !=
                    binding.canonical_name) {
                    return reject();
                }

                duplicate = true;
                break;
            }

            if (duplicate) {
                continue;
            }

            ++declaration;

            types.push_back({
                store_qualified(
                    binding.scope_name,
                    binding.name),
                store_exact(binding.canonical_name),
                {
                    source_id{1},
                    source_declaration_id{declaration}
                }
            });
        }

        initialized = true;
        return {};
    }
    catch (...) {
        reset_candidate();
        return {status_code::initialization_failed};
    }
}

bool source_interface_storage::find_constant_recursive(
    std::string_view scope,
    std::string_view name,
    integral_constant& output) const noexcept {

    for (auto position = constants.rbegin();
         position != constants.rend();
         ++position) {
        if (qualified_equal(
                spelling(position->lookup_name),
                scope,
                name)) {
            output = position->value;
            return true;
        }
    }

    for (const auto* imported : imports) {
        if (imported &&
            imported->find_constant_recursive(
                scope,
                name,
                output)) {
            return true;
        }
    }

    return false;
}

bool source_interface_storage::find_type_recursive(
    std::string_view scope,
    std::string_view name,
    source_entity_ref& entity,
    std::string_view& canonical) const noexcept {

    entity = {};
    canonical = {};

    for (auto position = types.rbegin();
         position != types.rend();
         ++position) {
        if (!qualified_equal(
                spelling(position->lookup_name),
                scope,
                name)) {
            continue;
        }

        const auto value =
            spelling(position->canonical_name);

        if (value.empty() ||
            !position->entity) {
            return false;
        }

        entity = position->entity;
        canonical = value;
        return true;
    }

    for (const auto* imported : imports) {
        if (imported &&
            imported->find_type_recursive(
                scope,
                name,
                entity,
                canonical)) {
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

    if (single &&
        single->find_constant_recursive(
            scope,
            name,
            output)) {
        return {};
    }

    for (const auto* dependency : dependencies) {
        if (dependency &&
            dependency->find_constant_recursive(
                scope,
                name,
                output)) {
            return {};
        }
    }

    for (const auto& imported : positional_imports) {
        if (imported.visible_from <= source_offset &&
            imported.storage &&
            imported.storage->find_constant_recursive(
                scope,
                name,
                output)) {
            return {};
        }
    }

    return {status_code::configuration_failed};
}

status source_environment::find_type_exact(
    std::string_view scope,
    std::string_view name,
    std::string_view& canonical) const noexcept {

    return find_type_exact(
        scope,
        name,
        (std::numeric_limits<std::uint32_t>::max)(),
        canonical);
}

status source_environment::find_type_exact(
    std::string_view scope,
    std::string_view name,
    std::uint32_t source_offset,
    std::string_view& canonical) const noexcept {

    source_entity_ref entity;

    return find_type_exact(
        scope,
        name,
        source_offset,
        entity,
        canonical);
}

status source_environment::find_type_exact(
    std::string_view scope,
    std::string_view name,
    std::uint32_t source_offset,
    source_entity_ref& entity,
    std::string_view& canonical) const noexcept {

    entity = {};
    canonical = {};

    if (name.empty()) {
        return {status_code::configuration_failed};
    }

    if (single &&
        single->find_type_recursive(
            scope,
            name,
            entity,
            canonical)) {
        return {};
    }

    for (const auto* dependency : dependencies) {
        if (dependency &&
            dependency->find_type_recursive(
                scope,
                name,
                entity,
                canonical)) {
            return {};
        }
    }

    for (const auto& imported : positional_imports) {
        if (imported.visible_from <= source_offset &&
            imported.storage &&
            imported.storage->find_type_recursive(
                scope,
                name,
                entity,
                canonical)) {
            return {};
        }
    }

    return {status_code::configuration_failed};
}

} // namespace cw::server