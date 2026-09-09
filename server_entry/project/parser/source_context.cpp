#include "source_context.hpp"

#include "../string/string_hash.hpp"

#include <limits>

namespace cw::server {
namespace {

constexpr std::size_t minimum_index_capacity = 16;

std::size_t hash_append(
    std::size_t hash,
    std::string_view value) noexcept {

    constexpr std::size_t prime =
        sizeof(std::size_t) == 8
            ? std::size_t{1099511628211ull}
            : std::size_t{16777619u};

    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }

    return hash;
}

std::size_t hash_begin() noexcept {
    return
        sizeof(std::size_t) == 8
            ? std::size_t{1469598103934665603ull}
            : std::size_t{2166136261u};
}

std::size_t hash_name(std::string_view value) noexcept {
    return hash_append(hash_begin(), value);
}

std::size_t hash_qualified(
    std::string_view scope,
    std::string_view name) noexcept {

    auto hash = hash_begin();

    if (!scope.empty()) {
        hash = hash_append(hash, scope);
        hash = hash_append(hash, "::");
    }

    return hash_append(hash, name);
}

bool qualified_equal(
    std::string_view canonical,
    std::string_view scope,
    std::string_view name) noexcept {

    if (name.empty()) {
        return false;
    }

    if (scope.empty()) {
        return canonical == name;
    }

    if (canonical.size() !=
        scope.size() + 2 + name.size()) {
        return false;
    }

    return
        canonical.substr(0, scope.size()) == scope &&
        canonical[scope.size()] == ':' &&
        canonical[scope.size() + 1] == ':' &&
        canonical.substr(scope.size() + 2) == name;
}

std::size_t next_index_capacity(
    std::size_t current,
    std::size_t required) noexcept {

    const auto maximum =
        (std::numeric_limits<std::size_t>::max)();

    auto capacity =
        current == 0
            ? minimum_index_capacity
            : current;

    while (required > capacity / 2) {
        if (capacity > maximum / 2) {
            return 0;
        }

        capacity *= 2;
    }

    return capacity;
}

} // namespace

status source_context::store_name(
    std::string_view value,
    source_name_ref& result) noexcept {

    result = {};

    if (value.empty()) {
        return {status_code::configuration_failed};
    }

    const auto maximum =
        static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)());

    if (value.size() > maximum ||
        names.size() > maximum - value.size() ||
        stored_name_count ==
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::initialization_failed};
    }

    const auto original_size =
        names.size();

    const auto hash =
        string_binding_hash(value);

    try {
        result = {
            static_cast<std::uint32_t>(original_size),
            static_cast<std::uint32_t>(value.size()),
            stored_name_count + 1
        };

        names.insert(
            names.end(),
            value.begin(),
            value.end());

        try {
            name_hashes.push_back(
                hash);
        }
        catch (...) {
            names.resize(
                original_size);
            throw;
        }

        ++stored_name_count;
        return {};
    }
    catch (...) {
        result = {};
        return {status_code::initialization_failed};
    }
}
status source_context::store_qualified_name(
    std::string_view scope,
    std::string_view local,
    source_name_ref& result) noexcept {

    result = {};

    if (local.empty()) {
        return {status_code::configuration_failed};
    }

    const auto separator =
        scope.empty()
            ? std::size_t{0}
            : std::size_t{2};

    const auto maximum =
        static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)());

    if (scope.size() > maximum ||
        local.size() > maximum - scope.size() ||
        separator > maximum - scope.size() - local.size() ||
        stored_name_count ==
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::initialization_failed};
    }

    const auto count =
        scope.size() +
        separator +
        local.size();

    if (names.size() > maximum - count) {
        return {status_code::initialization_failed};
    }

    const auto original_size =
        names.size();

    try {
        result = {
            static_cast<std::uint32_t>(original_size),
            static_cast<std::uint32_t>(count),
            stored_name_count + 1
        };

        if (!scope.empty()) {
            names.insert(
                names.end(),
                scope.begin(),
                scope.end());

            names.push_back(':');
            names.push_back(':');
        }

        names.insert(
            names.end(),
            local.begin(),
            local.end());

        const std::string_view canonical{
            names.data() + original_size,
            count
        };

        try {
            name_hashes.push_back(
                string_binding_hash(
                    canonical));
        }
        catch (...) {
            names.resize(
                original_size);
            throw;
        }

        ++stored_name_count;
        return {};
    }
    catch (...) {
        names.resize(original_size);
        result = {};
        return {status_code::initialization_failed};
    }
}
status source_context::resolve_name(
    source_name_ref reference,
    std::string_view& output) const noexcept {

    output = {};

    const auto end =
        std::uint64_t{reference.offset} +
        reference.length;

    if (!reference ||
        reference.index > stored_name_count ||
        end > names.size()) {
        return {status_code::configuration_failed};
    }

    output = {
        names.data() + reference.offset,
        reference.length
    };

    return {};
}
status source_context::ensure_type_index(
    std::size_t required) noexcept {

    const auto capacity =
        next_index_capacity(
            type_index.size(),
            required);

    if (capacity == 0) {
        return {status_code::initialization_failed};
    }

    if (capacity == type_index.size()) {
        return {};
    }

    try {
        std::vector<std::uint32_t> rebuilt(
            capacity,
            0);

        const auto mask =
            capacity - 1;

        for (std::size_t index = 0;
             index < type_declarations.size();
             ++index) {
            std::string_view canonical;

            auto result =
                resolve_name(
                    type_declarations[index].canonical_name,
                    canonical);

            if (!result.ok()) {
                return result;
            }

            auto slot =
                hash_name(canonical) &
                mask;

            for (;;) {
                const auto existing =
                    rebuilt[slot];

                if (existing == 0) {
                    rebuilt[slot] =
                        static_cast<std::uint32_t>(
                            index + 1);
                    break;
                }

                std::string_view existing_name;

                result =
                    resolve_name(
                        type_declarations[
                            existing - 1].canonical_name,
                        existing_name);

                if (!result.ok()) {
                    return result;
                }

                if (existing_name == canonical) {
                    rebuilt[slot] =
                        static_cast<std::uint32_t>(
                            index + 1);
                    break;
                }

                slot =
                    (slot + 1) &
                    mask;
            }
        }

        type_index.swap(rebuilt);
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status source_context::declare_type(
    source_id source,
    source_name_ref canonical_name,
    source_text_range name_range,
    source_entity_ref& result) noexcept {

    result = {};

    if (!source ||
        !canonical_name ||
        type_declarations.size() >=
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::configuration_failed};
    }

    std::string_view canonical;

    auto status_value =
        resolve_name(
            canonical_name,
            canonical);

    if (!status_value.ok() ||
        canonical.empty()) {
        return status_value.ok()
            ? status{status_code::configuration_failed}
            : status_value;
    }

    if (type_index_active) {
        status_value =
            ensure_type_index(
                type_declarations.size() + 1);

        if (!status_value.ok()) {
            return status_value;
        }
    }

    try {
        result = {
            source,
            source_declaration_id{
                static_cast<std::uint32_t>(
                    type_declarations.size() + 1)}
        };

        type_declarations.push_back({
            result,
            canonical_name,
            name_range
        });

        // No source-language type reference has occurred yet. Declarations stay
        // dense and append-only; no lookup index work is performed.
        if (!type_index_active) {
            return {};
        }

        const auto mask =
            type_index.size() - 1;

        auto slot =
            hash_name(canonical) &
            mask;

        for (;;) {
            const auto existing =
                type_index[slot];

            if (existing == 0) {
                type_index[slot] =
                    result.declaration.value();
                return {};
            }

            std::string_view existing_name;

            status_value =
                resolve_name(
                    type_declarations[
                        existing - 1].canonical_name,
                    existing_name);

            if (!status_value.ok()) {
                return status_value;
            }

            if (existing_name == canonical) {
                type_index[slot] =
                    result.declaration.value();
                return {};
            }

            slot =
                (slot + 1) &
                mask;
        }
    }
    catch (...) {
        result = {};
        return {status_code::initialization_failed};
    }
}

status source_context::find_type(
    std::string_view scope,
    std::string_view name,
    source_entity_ref& entity,
    source_name_ref& canonical_name) noexcept {

    entity = {};
    canonical_name = {};

    if (name.empty() ||
        type_declarations.empty()) {
        return {status_code::configuration_failed};
    }

    if (!type_index_active) {
        const auto result =
            ensure_type_index(
                type_declarations.size());

        if (!result.ok()) {
            return result;
        }

        type_index_active = true;
    }

    const auto mask =
        type_index.size() - 1;

    auto slot =
        hash_qualified(
            scope,
            name) &
        mask;

    for (std::size_t probe = 0;
         probe < type_index.size();
         ++probe) {
        const auto raw =
            type_index[slot];

        if (raw == 0) {
            return {status_code::configuration_failed};
        }

        if (raw > type_declarations.size()) {
            return {status_code::initialization_failed};
        }

        const auto& declaration =
            type_declarations[raw - 1];

        std::string_view canonical;

        const auto result =
            resolve_name(
                declaration.canonical_name,
                canonical);

        if (!result.ok()) {
            return result;
        }

        if (qualified_equal(
                canonical,
                scope,
                name)) {
            entity = declaration.entity;
            canonical_name =
                declaration.canonical_name;

            return entity
                ? status{}
                : status{
                    status_code::configuration_failed};
        }

        slot =
            (slot + 1) &
            mask;
    }

    return {status_code::configuration_failed};
}

status source_context::ensure_static_object_index(
    std::size_t required) noexcept {

    const auto capacity =
        next_index_capacity(
            static_object_index.size(),
            required);

    if (capacity == 0) {
        return {status_code::initialization_failed};
    }

    if (capacity == static_object_index.size()) {
        return {};
    }

    try {
        std::vector<std::uint32_t> rebuilt(
            capacity,
            0);

        const auto mask =
            capacity - 1;

        for (std::size_t index = 0;
             index < static_objects.size();
             ++index) {
            std::string_view canonical;

            auto result =
                resolve_name(
                    static_objects[index].canonical_name,
                    canonical);

            if (!result.ok() ||
                canonical.empty()) {
                return result.ok()
                    ? status{status_code::configuration_failed}
                    : result;
            }

            auto slot =
                hash_name(canonical) &
                mask;

            while (rebuilt[slot] != 0) {
                slot =
                    (slot + 1) &
                    mask;
            }

            rebuilt[slot] =
                static_cast<std::uint32_t>(
                    index + 1);
        }

        static_object_index.swap(rebuilt);
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status source_context::declare_static_object(
    const static_object_source_fact& fact,
    std::uint32_t& object) noexcept {

    object = 0;

    if (!fact.canonical_name ||
        (fact.builtin && fact.type_entity) ||
        (!fact.builtin && !fact.type_entity) ||
        static_objects.size() >=
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::configuration_failed};
    }

    std::string_view canonical;

    auto result =
        resolve_name(
            fact.canonical_name,
            canonical);

    if (!result.ok() ||
        canonical.empty()) {
        return result.ok()
            ? status{status_code::configuration_failed}
            : result;
    }

    result =
        ensure_static_object_index(
            static_objects.size() + 1);

    if (!result.ok()) {
        return result;
    }

    const auto mask =
        static_object_index.size() - 1;

    auto slot =
        hash_name(canonical) &
        mask;

    for (;;) {
        const auto raw =
            static_object_index[slot];

        if (raw == 0) {
            break;
        }

        if (raw > static_objects.size()) {
            return {status_code::initialization_failed};
        }

        std::string_view existing;

        result =
            resolve_name(
                static_objects[
                    raw - 1].canonical_name,
                existing);

        if (!result.ok()) {
            return result;
        }

        if (existing == canonical) {
            return {status_code::configuration_failed};
        }

        slot =
            (slot + 1) &
            mask;
    }

    try {
        static_objects.push_back(fact);

        object =
            static_cast<std::uint32_t>(
                static_objects.size());

        static_object_index[slot] = object;
        return {};
    }
    catch (...) {
        object = 0;
        return {status_code::initialization_failed};
    }
}

status source_context::find_static_object(
    std::string_view scope,
    std::string_view name,
    std::uint32_t& object) const noexcept {

    object = 0;

    if (name.empty() ||
        static_object_index.empty()) {
        return {status_code::configuration_failed};
    }

    const auto mask =
        static_object_index.size() - 1;

    auto slot =
        hash_qualified(
            scope,
            name) &
        mask;

    for (std::size_t probe = 0;
         probe < static_object_index.size();
         ++probe) {
        const auto raw =
            static_object_index[slot];

        if (raw == 0) {
            return {status_code::configuration_failed};
        }

        if (raw > static_objects.size()) {
            return {status_code::initialization_failed};
        }

        const auto& existing =
            static_objects[raw - 1];

        std::string_view canonical;

        const auto result =
            resolve_name(
                existing.canonical_name,
                canonical);

        if (!result.ok()) {
            return result;
        }

        if (qualified_equal(
                canonical,
                scope,
                name)) {
            object = raw;
            return {};
        }

        slot =
            (slot + 1) &
            mask;
    }

    return {status_code::configuration_failed};
}

status source_context::ensure_constant_index(
    std::size_t required) noexcept {

    const auto capacity =
        next_index_capacity(
            constant_index.size(),
            required);

    if (capacity == 0) {
        return {status_code::initialization_failed};
    }

    if (capacity == constant_index.size()) {
        return {};
    }

    try {
        std::vector<std::uint32_t> rebuilt(
            capacity,
            0);

        const auto mask =
            capacity - 1;

        for (std::size_t index = 0;
             index < constant_symbols.size();
             ++index) {
            const auto& symbol =
                constant_symbols[index];

            std::string_view scope;
            std::string_view name;

            if (symbol.scope) {
                auto result =
                    resolve_name(
                        symbol.scope,
                        scope);

                if (!result.ok()) {
                    return result;
                }
            }

            auto result =
                resolve_name(
                    symbol.name,
                    name);

            if (!result.ok()) {
                return result;
            }

            auto slot =
                hash_qualified(
                    scope,
                    name) &
                mask;

            while (rebuilt[slot] != 0) {
                slot =
                    (slot + 1) &
                    mask;
            }

            rebuilt[slot] =
                static_cast<std::uint32_t>(
                    index + 1);
        }

        constant_index.swap(rebuilt);
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status source_context::declare_constant(
    source_name_ref scope_ref,
    source_name_ref name_ref,
    integral_constant value,
    source_declaration_result& declaration) noexcept {

    declaration = source_declaration_result::existing;

    if (!name_ref ||
        !is_integral(value.type)) {
        return {status_code::configuration_failed};
    }

    std::string_view scope;
    std::string_view name;

    if (scope_ref) {
        auto result =
            resolve_name(
                scope_ref,
                scope);

        if (!result.ok()) {
            return result;
        }
    }

    auto result =
        resolve_name(
            name_ref,
            name);

    if (!result.ok()) {
        return result;
    }

    result =
        ensure_constant_index(
            constant_symbols.size() + 1);

    if (!result.ok()) {
        return result;
    }

    std::size_t slot = 0;
    result = find_constant_slot(scope, name, slot);
    if (!result.ok()) return result;
    if (constant_index[slot] != 0) return {};

    if (constant_symbols.size() >=
        (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::initialization_failed};
    }

    try {
        constant_symbols.push_back({scope_ref, name_ref, value});
        constant_index[slot] = static_cast<std::uint32_t>(constant_symbols.size());
        declaration = source_declaration_result::inserted;
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status source_context::find_constant_slot(
    std::string_view scope,
    std::string_view name,
    std::size_t& slot) const noexcept {

    if (name.empty() || constant_index.empty()) {
        return {status_code::configuration_failed};
    }

    const auto mask =
        constant_index.size() - 1;

    slot =
        hash_qualified(
            scope,
            name) &
        mask;

    for (;;) {
        const auto raw =
            constant_index[slot];

        if (raw == 0) {
            return {};
        }

        if (raw > constant_symbols.size()) {
            return {status_code::initialization_failed};
        }

        const auto& existing =
            constant_symbols[raw - 1];

        std::string_view existing_scope;
        std::string_view existing_name;

        if (existing.scope) {
            const auto result =
                resolve_name(
                    existing.scope,
                    existing_scope);

            if (!result.ok()) {
                return result;
            }
        }

        const auto result =
            resolve_name(
                existing.name,
                existing_name);

        if (!result.ok()) {
            return result;
        }

        if (existing_scope == scope &&
            existing_name == name) {
            return {};
        }

        slot =
            (slot + 1) &
            mask;
    }

}

status source_context::find_constant_exact(
    std::string_view scope,
    std::string_view name,
    integral_constant& output) const noexcept {

    output = {};
    std::size_t slot = 0;
    const auto result = find_constant_slot(scope, name, slot);
    if (!result.ok()) return result;
    const auto raw = constant_index[slot];
    if (raw == 0) return {status_code::configuration_failed};
    output = constant_symbols[raw - 1].value;
    return {};
}

status source_context::release_facts(
    source_id source,
    source_facts_storage& output) noexcept {

    if (!source) {
        return {status_code::configuration_failed};
    }

    if (name_hashes.size() !=
        stored_name_count) {
        return {
            status_code::initialization_failed
        };
    }

    output.reset();

    try {
        output.source = source;
        output.stored_name_count = stored_name_count;
        output.names = std::move(names);
        output.name_hashes =
            std::move(name_hashes);
        output.enum_values = std::move(enum_values);
        output.enums = std::move(enums);
        output.modifiers = std::move(type_modifiers);
        output.members = std::move(aggregate_members);
        output.aggregates = std::move(aggregates);
        output.static_objects = std::move(static_objects);
        output.construction_bindings =
            std::move(aggregate_construction_bindings);

        stored_name_count = 0;
        return {};
    }
    catch (...) {
        output.reset();
        return {status_code::initialization_failed};
    }
}

std::span<const enum_value_source_fact> source_context::enumerators(
    const enum_declaration_source_fact& declaration) const noexcept {

    if (declaration.enumerator_count == 0) {
        return {};
    }

    const auto end =
        std::uint64_t{declaration.enumerator_offset} +
        declaration.enumerator_count;

    if (end > enum_values.size()) {
        return {};
    }

    return {
        enum_values.data() + declaration.enumerator_offset,
        declaration.enumerator_count
    };
}

std::span<const member_declaration_source_fact> source_context::members(
    const aggregate_declaration_source_fact& declaration) const noexcept {

    if (declaration.member_count == 0) {
        return {};
    }

    const auto end =
        std::uint64_t{declaration.member_offset} +
        declaration.member_count;

    if (end > aggregate_members.size()) {
        return {};
    }

    return {
        aggregate_members.data() + declaration.member_offset,
        declaration.member_count
    };
}

std::span<const source_type_modifier> source_context::modifiers(
    const member_declaration_source_fact& member) const noexcept {

    if (member.modifier_count == 0) {
        return {};
    }

    const auto end =
        std::uint64_t{member.modifier_offset} +
        member.modifier_count;

    if (end > type_modifiers.size()) {
        return {};
    }

    return {
        type_modifiers.data() + member.modifier_offset,
        member.modifier_count
    };
}

std::span<const source_type_modifier> source_context::modifiers(
    const static_object_source_fact& object) const noexcept {

    if (object.modifier_count == 0) {
        return {};
    }

    const auto end =
        std::uint64_t{object.modifier_offset} +
        object.modifier_count;

    if (end > type_modifiers.size()) {
        return {};
    }

    return {
        type_modifiers.data() + object.modifier_offset,
        object.modifier_count
    };
}

std::span<const construction_binding_source_fact>
source_context::construction_bindings(
    const aggregate_declaration_source_fact& declaration) const noexcept {

    if (declaration.construction_binding_count == 0) {
        return {};
    }

    const auto end =
        std::uint64_t{declaration.construction_binding_offset} +
        declaration.construction_binding_count;

    if (end >
        aggregate_construction_bindings.size()) {
        return {};
    }

    return {
        aggregate_construction_bindings.data() +
            declaration.construction_binding_offset,
        declaration.construction_binding_count
    };
}

void source_context::reset() noexcept {
    names.clear();
    name_hashes.clear();
    stored_name_count = 0;
    tokens.clear();
    type_declarations.clear();
    type_index.clear();
    type_index_active = false;
    constant_symbols.clear();
    constant_index.clear();
    enum_values.clear();
    enums.clear();
    aggregates.clear();
    aggregate_members.clear();
    type_modifiers.clear();
    static_objects.clear();
    static_object_index.clear();
    aggregate_construction_bindings.clear();
    diagnostics.clear();
}

} // namespace cw::server
