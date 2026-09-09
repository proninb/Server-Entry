#include "implementation_context.hpp"

#include "../string/string_hash.hpp"

#include <limits>
#include <utility>

namespace cw::server {
namespace {

constexpr std::size_t minimum_index_capacity = 16;

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

status implementation_facts_storage::resolve_name(
    source_name_ref reference,
    std::string_view& output) const noexcept {

    output = {};

    const auto end =
        std::uint64_t{reference.offset} +
        reference.length;

    if (!reference ||
        reference.index > stored_name_count ||
        end > names.size()) {
        return {
            status_code::configuration_failed
        };
    }

    output = {
        names.data() + reference.offset,
        reference.length
    };

    return {};
}

void implementation_facts_storage::reset() noexcept {
    source = {};
    stored_name_count = 0;
    names.clear();
    objects.clear();
    object_dimensions.clear();
    paths.clear();
    path_steps.clear();
    literals.clear();
    literal_bytes.clear();
    operations.clear();
}

status implementation_context::store_name(
    std::string_view value,
    source_name_ref& result) noexcept {

    result = {};

    if (value.empty() ||
        value.size() >
            (std::numeric_limits<std::uint32_t>::max)() ||
        names.size() >
            (std::numeric_limits<std::uint32_t>::max)() -
                value.size() ||
        stored_name_count ==
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {
            status_code::configuration_failed
        };
    }

    try {
        result = {
            static_cast<std::uint32_t>(
                names.size()),
            static_cast<std::uint32_t>(
                value.size()),
            stored_name_count + 1
        };

        names.insert(
            names.end(),
            value.begin(),
            value.end());

        ++stored_name_count;
        return {};
    }
    catch (...) {
        result = {};
        return {
            status_code::initialization_failed
        };
    }
}

status implementation_context::resolve_name(
    source_name_ref reference,
    std::string_view& output) const noexcept {

    output = {};

    const auto end =
        std::uint64_t{reference.offset} +
        reference.length;

    if (!reference ||
        reference.index > stored_name_count ||
        end > names.size()) {
        return {
            status_code::configuration_failed
        };
    }

    output = {
        names.data() + reference.offset,
        reference.length
    };

    return {};
}

status implementation_context::ensure_object_index(
    std::size_t required) noexcept {

    const auto capacity =
        next_index_capacity(
            object_index.size(),
            required);

    if (capacity == 0) {
        return {
            status_code::initialization_failed
        };
    }

    if (capacity == object_index.size()) {
        return {};
    }

    try {
        std::vector<std::uint32_t> rebuilt(
            capacity,
            0);

        const auto mask =
            capacity - 1;

        for (std::size_t index = 0;
             index < objects.size();
             ++index) {
            std::string_view spelling;

            auto result =
                resolve_name(
                    objects[index].name,
                    spelling);

            if (!result.ok()) {
                return result;
            }

            auto slot =
                static_cast<std::size_t>(
                    string_binding_hash(spelling)) &
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

        object_index.swap(rebuilt);
        return {};
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

status implementation_context::declare_object(
    std::string_view spelling,
    const implementation_object_fact& fact,
    std::uint32_t& object) noexcept {

    object = 0;

    if (spelling.empty() ||
        !fact.name ||
        !fact.type ||
        objects.size() >=
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {
            status_code::configuration_failed
        };
    }

    auto result =
        ensure_object_index(
            objects.size() + 1);

    if (!result.ok()) {
        return result;
    }

    const auto mask =
        object_index.size() - 1;

    auto slot =
        static_cast<std::size_t>(
            string_binding_hash(spelling)) &
        mask;

    for (;;) {
        const auto raw =
            object_index[slot];

        if (raw == 0) {
            break;
        }

        if (raw > objects.size()) {
            return {
                status_code::initialization_failed
            };
        }

        std::string_view existing;

        result =
            resolve_name(
                objects[raw - 1].name,
                existing);

        if (!result.ok()) {
            return result;
        }

        if (existing == spelling) {
            return {
                status_code::configuration_failed
            };
        }

        slot =
            (slot + 1) &
            mask;
    }

    try {
        objects.push_back(fact);

        object =
            static_cast<std::uint32_t>(
                objects.size());

        object_index[slot] = object;
        return {};
    }
    catch (...) {
        object = 0;
        return {
            status_code::initialization_failed
        };
    }
}

status implementation_context::find_object(
    std::string_view spelling,
    std::uint32_t& object) const noexcept {

    object = 0;

    if (spelling.empty() ||
        object_index.empty()) {
        return {
            status_code::configuration_failed
        };
    }

    const auto mask =
        object_index.size() - 1;

    auto slot =
        static_cast<std::size_t>(
            string_binding_hash(spelling)) &
        mask;

    for (std::size_t probe = 0;
         probe < object_index.size();
         ++probe) {
        const auto raw =
            object_index[slot];

        if (raw == 0) {
            return {
                status_code::configuration_failed
            };
        }

        if (raw > objects.size()) {
            return {
                status_code::initialization_failed
            };
        }

        std::string_view existing;

        const auto result =
            resolve_name(
                objects[raw - 1].name,
                existing);

        if (!result.ok()) {
            return result;
        }

        if (existing == spelling) {
            object = raw;
            return {};
        }

        slot =
            (slot + 1) &
            mask;
    }

    return {
        status_code::configuration_failed
    };
}

status implementation_context::release_facts(
    source_id source,
    implementation_facts_storage& output) noexcept {

    if (!source) {
        return {
            status_code::configuration_failed
        };
    }

    output.reset();

    try {
        output.source = source;
        output.stored_name_count =
            stored_name_count;
        output.names =
            std::move(names);
        output.objects =
            std::move(objects);
        output.object_dimensions =
            std::move(object_dimensions);
        output.paths =
            std::move(paths);
        output.path_steps =
            std::move(path_steps);
        output.literals =
            std::move(literals);
        output.literal_bytes =
            std::move(literal_bytes);
        output.operations =
            std::move(operations);

        stored_name_count = 0;
        object_index.clear();
        tokens.clear();
        diagnostics.clear();

        return {};
    }
    catch (...) {
        output.reset();
        return {
            status_code::initialization_failed
        };
    }
}

void implementation_context::reset() noexcept {
    stored_name_count = 0;
    names.clear();
    object_index.clear();
    tokens.clear();
    objects.clear();
    object_dimensions.clear();
    paths.clear();
    path_steps.clear();
    literals.clear();
    literal_bytes.clear();
    operations.clear();
    diagnostics.clear();
}

} // namespace cw::server
