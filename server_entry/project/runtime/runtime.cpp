#include "runtime.hpp"

#include "../graph/graph.hpp"

#include <bit>
#include <cassert>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace cw::server {
namespace {

static_assert(sizeof(std::uintptr_t) == 8);
static_assert(std::endian::native == std::endian::little);
static_assert(
    std::is_nothrow_move_assignable_v<implementation_facts_storage>);
static_assert(
    std::is_nothrow_move_constructible_v<implementation_facts_storage>);

[[nodiscard]] bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {

    if (left >
        (std::numeric_limits<std::uint64_t>::max)() - right) {
        output = 0;
        return false;
    }

    output = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {

    if (left != 0 &&
        right >
            (std::numeric_limits<std::uint64_t>::max)() / left) {
        output = 0;
        return false;
    }

    output = left * right;
    return true;
}

[[nodiscard]] bool align_up(
    std::uint64_t value,
    std::uint32_t alignment,
    std::uint64_t& output) noexcept {

    output = 0;

    if (alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
        return false;
    }

    const auto mask =
        static_cast<std::uint64_t>(alignment - 1);

    if (value >
        (std::numeric_limits<std::uint64_t>::max)() - mask) {
        return false;
    }

    output = (value + mask) & ~mask;
    return true;
}

} // namespace

struct runtime::materialized_state {
    struct storage_record {
        TypeRef type{};
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::uint64_t element_count = 1;
    };

    struct aligned_storage {
        std::vector<std::max_align_t> units;
        std::size_t byte_size = 0;

        [[nodiscard]] std::byte* data() noexcept {
            return reinterpret_cast<std::byte*>(units.data());
        }

        [[nodiscard]] const std::byte* data() const noexcept {
            return reinterpret_cast<const std::byte*>(units.data());
        }

        [[nodiscard]] status resize_zeroed(
            std::uint64_t size) {

            if (size >
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
                return {status_code::initialization_failed};
            }

            const auto bytes =
                static_cast<std::size_t>(size);

            const auto unit_size =
                sizeof(std::max_align_t);

            const auto count =
                bytes / unit_size +
                (bytes % unit_size != 0 ? 1 : 0);

            units.resize(count);
            byte_size = bytes;

            if (byte_size != 0) {
                std::memset(
                    data(),
                    0,
                    byte_size);
            }

            return {};
        }
    };

    struct source_storage {
        source_id source{};
        std::vector<storage_record> objects;
        aligned_storage bytes;
    };

    static_assert(
        std::is_nothrow_move_constructible_v<source_storage>);
    static_assert(
        std::is_nothrow_move_assignable_v<source_storage>);

    struct resolved_path {
        std::byte* address = nullptr;
        TypeRef type{};
        std::uint32_t remaining_object_dimensions = 0;
    };

    const graph* graph_state = nullptr;
    std::vector<storage_record> static_objects;
    aligned_storage static_bytes;
    std::vector<source_storage> sources;
    std::vector<source_storage> prepared_sources;

    [[nodiscard]] bool storage_alignment_supported(
        std::uint32_t alignment) const noexcept {

        return
            alignment != 0 &&
            alignment <= alignof(std::max_align_t);
    }

    [[nodiscard]] status append_storage_record(
        TypeRef type,
        std::uint64_t element_count,
        std::uint64_t& cursor,
        storage_record& output) const noexcept {

        output = {};

        if (!graph_state ||
            !type ||
            element_count == 0) {
            return {status_code::configuration_failed};
        }

        type_layout_record layout;

        if (!graph_state->layout(type, layout) ||
            !storage_alignment_supported(layout.alignment)) {
            return {status_code::configuration_failed};
        }

        std::uint64_t size = 0;
        std::uint64_t offset = 0;
        std::uint64_t end = 0;

        if (!checked_multiply(
                layout.size,
                element_count,
                size) ||
            !align_up(
                cursor,
                layout.alignment,
                offset) ||
            !checked_add(
                offset,
                size,
                end)) {
            return {status_code::initialization_failed};
        }

        output = {
            type,
            offset,
            size,
            element_count
        };

        cursor = end;
        return {};
    }

    [[nodiscard]] status write_reference(
        std::byte* slot,
        std::byte* target) const noexcept {

        if (!slot || !target) {
            return {status_code::configuration_failed};
        }

        const auto address =
            reinterpret_cast<std::uintptr_t>(target);

        std::memcpy(
            slot,
            &address,
            sizeof(address));

        return {};
    }

    [[nodiscard]] status dereference_value(
        std::byte*& address,
        TypeRef& type) const noexcept {

        if (!graph_state || !address || !type) {
            return {status_code::configuration_failed};
        }

        for (std::size_t depth = 0;
             depth < graph_state->derived_type_count() + 2;
             ++depth) {
            const auto* derived =
                graph_state->derived(type);

            if (!derived ||
                (derived->kind !=
                     derived_type_kind::lvalue_reference &&
                 derived->kind !=
                     derived_type_kind::rvalue_reference)) {
                return {};
            }

            std::uintptr_t target = 0;

            std::memcpy(
                &target,
                address,
                sizeof(target));

            if (target == 0) {
                return {status_code::configuration_failed};
            }

            address =
                reinterpret_cast<std::byte*>(target);

            type = derived->child;
        }

        return {status_code::configuration_failed};
    }

    [[nodiscard]] status construct_type(
        TypeRef type,
        std::byte* address,
        std::size_t depth = 0) noexcept {

        if (!graph_state ||
            !type ||
            !address ||
            depth >
                graph_state->user_type_count() +
                graph_state->derived_type_count() +
                64) {
            return {status_code::configuration_failed};
        }

        builtin_type builtin;

        if (graph_state->builtin(type, builtin)) {
            return builtin != builtin_type::void_type
                ? status{}
                : status{status_code::configuration_failed};
        }

        type_handle named;

        if (graph_state->named(type, named)) {
            const auto* entry =
                graph_state->find(named);

            if (!entry) {
                return {status_code::configuration_failed};
            }

            if (entry->kind ==
                user_type_kind::enumeration) {
                return {};
            }

            if (entry->kind !=
                user_type_kind::aggregate) {
                return {status_code::configuration_failed};
            }

            const auto members =
                graph_state->members(named);

            const auto bindings =
                graph_state->construction_bindings(named);

            for (std::uint32_t raw = 1;
                 raw <= members.size();
                 ++raw) {
                const member_index member{raw};
                const auto* layout =
                    graph_state->member_layout(
                        named,
                        member);

                if (!layout) {
                    return {status_code::configuration_failed};
                }

                auto* member_address =
                    address + layout->offset;

                const auto member_type =
                    members[raw - 1].type;

                const auto* derived =
                    graph_state->derived(member_type);

                if (derived &&
                    (derived->kind ==
                         derived_type_kind::lvalue_reference ||
                     derived->kind ==
                         derived_type_kind::rvalue_reference)) {
                    if (derived->kind !=
                        derived_type_kind::lvalue_reference) {
                        return {status_code::configuration_failed};
                    }

                    const construction_binding_record* binding = nullptr;

                    for (const auto& candidate : bindings) {
                        if (candidate.member == member) {
                            binding = &candidate;
                            break;
                        }
                    }

                    if (!binding ||
                        binding->static_object == 0 ||
                        binding->static_object > static_objects.size()) {
                        return {status_code::configuration_failed};
                    }

                    const auto& object =
                        static_objects[
                            binding->static_object - 1];

                    if (derived->child != object.type ||
                        object.offset >= static_bytes.byte_size) {
                        return {status_code::configuration_failed};
                    }

                    auto* target =
                        static_bytes.data() +
                        object.offset;

                    const auto result =
                        write_reference(
                            member_address,
                            target);

                    if (!result.ok()) {
                        return result;
                    }

                    continue;
                }

                const auto result =
                    construct_type(
                        member_type,
                        member_address,
                        depth + 1);

                if (!result.ok()) {
                    return result;
                }
            }

            return {};
        }

        const auto* derived =
            graph_state->derived(type);

        if (!derived) {
            return {status_code::configuration_failed};
        }

        if (derived->kind ==
            derived_type_kind::pointer) {
            return {};
        }

        if (derived->kind !=
            derived_type_kind::array ||
            derived->payload == 0) {
            return {status_code::configuration_failed};
        }

        type_layout_record child_layout;

        if (!graph_state->layout(
                derived->child,
                child_layout)) {
            return {status_code::configuration_failed};
        }

        for (std::uint64_t index = 0;
             index < derived->payload;
             ++index) {
            std::uint64_t offset = 0;

            if (!checked_multiply(
                    index,
                    child_layout.size,
                    offset)) {
                return {status_code::initialization_failed};
            }

            const auto result =
                construct_type(
                    derived->child,
                    address + offset,
                    depth + 1);

            if (!result.ok()) {
                return result;
            }
        }

        return {};
    }

    [[nodiscard]] status build_static_storage() {
        static_objects.clear();
        static_bytes.units.clear();
        static_bytes.byte_size = 0;

        if (!graph_state) {
            return {status_code::invalid_state};
        }

        const auto facts =
            graph_state->static_objects();

        static_objects.reserve(facts.size());

        std::uint64_t cursor = 0;

        for (const auto& fact : facts) {
            storage_record record;

            const auto result =
                append_storage_record(
                    fact.type,
                    1,
                    cursor,
                    record);

            if (!result.ok()) {
                return result;
            }

            static_objects.push_back(record);
        }

        auto result =
            static_bytes.resize_zeroed(cursor);

        if (!result.ok()) {
            return result;
        }

        for (const auto& object : static_objects) {
            result =
                construct_type(
                    object.type,
                    static_bytes.data() + object.offset);

            if (!result.ok()) {
                return result;
            }
        }

        return {};
    }

    [[nodiscard]] status object_element_count(
        const implementation_facts_storage& facts,
        const implementation_object_fact& object,
        std::uint64_t& output) const noexcept {

        output = 1;

        const auto begin =
            static_cast<std::size_t>(
                object.dimension_offset);

        const auto count =
            static_cast<std::size_t>(
                object.dimension_count);

        if (begin > facts.object_dimensions.size() ||
            count > facts.object_dimensions.size() - begin) {
            return {status_code::configuration_failed};
        }

        for (std::size_t index = 0;
             index < count;
             ++index) {
            const auto extent =
                facts.object_dimensions[begin + index];

            if (extent == 0 ||
                !checked_multiply(
                    output,
                    extent,
                    output)) {
                return {status_code::configuration_failed};
            }
        }

        return {};
    }

    [[nodiscard]] status remaining_object_stride(
        const implementation_facts_storage& facts,
        const implementation_object_fact& object,
        std::uint32_t consumed_dimensions,
        std::uint64_t& output) const noexcept {

        type_layout_record layout;

        if (!graph_state ||
            !graph_state->layout(
                object.type,
                layout)) {
            return {status_code::configuration_failed};
        }

        output = layout.size;

        const auto first =
            static_cast<std::size_t>(
                object.dimension_offset) +
            consumed_dimensions + 1;

        const auto end =
            static_cast<std::size_t>(
                object.dimension_offset) +
            object.dimension_count;

        if (end > facts.object_dimensions.size() ||
            first > end) {
            return {status_code::configuration_failed};
        }

        for (std::size_t index = first;
             index < end;
             ++index) {
            if (!checked_multiply(
                    output,
                    facts.object_dimensions[index],
                    output)) {
                return {status_code::initialization_failed};
            }
        }

        return {};
    }

    [[nodiscard]] status resolve_path(
        const implementation_facts_storage& facts,
        source_storage& source,
        std::uint32_t coordinate,
        resolved_path& output) const noexcept {

        output = {};

        if (!graph_state ||
            coordinate == 0 ||
            coordinate > facts.paths.size()) {
            return {status_code::configuration_failed};
        }

        const auto& path =
            facts.paths[coordinate - 1];

        if (!path.object_index ||
            path.object_index > facts.objects.size() ||
            path.object_index > source.objects.size()) {
            return {status_code::configuration_failed};
        }

        const auto& object =
            facts.objects[path.object_index - 1];

        const auto& storage =
            source.objects[path.object_index - 1];

        if (storage.offset >= source.bytes.byte_size ||
            storage.size >
                source.bytes.byte_size - storage.offset) {
            return {status_code::configuration_failed};
        }

        auto* address =
            source.bytes.data() + storage.offset;

        auto type = object.type;
        auto remaining_dimensions =
            object.dimension_count;
        std::uint32_t consumed_dimensions = 0;

        const auto step_begin =
            static_cast<std::size_t>(
                path.step_offset);

        const auto step_count =
            static_cast<std::size_t>(
                path.step_count);

        if (step_begin > facts.path_steps.size() ||
            step_count >
                facts.path_steps.size() - step_begin) {
            return {status_code::configuration_failed};
        }

        for (std::size_t index = 0;
             index < step_count;
             ++index) {
            const auto& step =
                facts.path_steps[step_begin + index];

            if (step.kind ==
                implementation_path_step_kind::index) {
                if (remaining_dimensions != 0) {
                    const auto dimension =
                        static_cast<std::size_t>(
                            object.dimension_offset) +
                        consumed_dimensions;

                    if (dimension >=
                            facts.object_dimensions.size() ||
                        step.index >=
                            facts.object_dimensions[dimension]) {
                        return {status_code::configuration_failed};
                    }

                    std::uint64_t stride = 0;
                    std::uint64_t offset = 0;

                    auto result =
                        remaining_object_stride(
                            facts,
                            object,
                            consumed_dimensions,
                            stride);

                    if (!result.ok() ||
                        !checked_multiply(
                            step.index,
                            stride,
                            offset)) {
                        return result.ok()
                            ? status{status_code::initialization_failed}
                            : result;
                    }

                    address += offset;
                    --remaining_dimensions;
                    ++consumed_dimensions;
                    continue;
                }

                auto result =
                    dereference_value(
                        address,
                        type);

                if (!result.ok()) {
                    return result;
                }

                const auto* derived =
                    graph_state->derived(type);

                if (!derived ||
                    derived->kind !=
                        derived_type_kind::array ||
                    step.index >= derived->payload) {
                    return {status_code::configuration_failed};
                }

                type_layout_record child_layout;
                std::uint64_t offset = 0;

                if (!graph_state->layout(
                        derived->child,
                        child_layout) ||
                    !checked_multiply(
                        step.index,
                        child_layout.size,
                        offset)) {
                    return {status_code::configuration_failed};
                }

                address += offset;
                type = derived->child;
                continue;
            }

            if (step.kind !=
                    implementation_path_step_kind::member ||
                remaining_dimensions != 0 ||
                !step.member) {
                return {status_code::configuration_failed};
            }

            auto result =
                dereference_value(
                    address,
                    type);

            if (!result.ok()) {
                return result;
            }

            type_handle owner;

            if (!graph_state->named(type, owner)) {
                return {status_code::configuration_failed};
            }

            const auto* member =
                graph_state->member(
                    owner,
                    step.member);

            const auto* layout =
                graph_state->member_layout(
                    owner,
                    step.member);

            if (!member || !layout) {
                return {status_code::configuration_failed};
            }

            address += layout->offset;
            type = member->type;
        }

        if (remaining_dimensions !=
                path.remaining_object_dimensions ||
            type != path.type) {
            return {status_code::configuration_failed};
        }

        output = {
            address,
            type,
            remaining_dimensions
        };

        return {};
    }

    [[nodiscard]] status resolve_value_path(
        const implementation_facts_storage& facts,
        source_storage& source,
        std::uint32_t coordinate,
        resolved_path& output) const noexcept {

        auto result =
            resolve_path(
                facts,
                source,
                coordinate,
                output);

        if (!result.ok() ||
            output.remaining_object_dimensions != 0) {
            return result.ok()
                ? status{status_code::configuration_failed}
                : result;
        }

        return dereference_value(
            output.address,
            output.type);
    }

    [[nodiscard]] bool copyable_value_type(
        TypeRef type) const noexcept {

        if (!graph_state || !type) {
            return false;
        }

        builtin_type builtin;

        if (graph_state->builtin(type, builtin)) {
            return builtin != builtin_type::void_type;
        }

        if (const auto* derived =
                graph_state->derived(type)) {
            return derived->kind ==
                derived_type_kind::pointer;
        }

        type_handle named;

        if (!graph_state->named(type, named)) {
            return false;
        }

        const auto* entry =
            graph_state->find(named);

        return entry &&
            entry->kind == user_type_kind::enumeration;
    }

    [[nodiscard]] status apply_string_literal(
        const implementation_facts_storage& facts,
        source_storage& source,
        const implementation_path_fact& path,
        const implementation_literal_fact& literal,
        std::uint32_t target_path) const noexcept {

        resolved_path target;

        auto result =
            resolve_path(
                facts,
                source,
                target_path,
                target);

        if (!result.ok()) {
            return result;
        }

        std::uint64_t capacity = 0;

        if (target.remaining_object_dimensions != 0) {
            if (target.remaining_object_dimensions != 1 ||
                !path.object_index ||
                path.object_index > facts.objects.size()) {
                return {status_code::configuration_failed};
            }

            const auto& object =
                facts.objects[path.object_index - 1];

            if (object.dimension_count <
                target.remaining_object_dimensions) {
                return {status_code::configuration_failed};
            }

            const auto consumed =
                object.dimension_count -
                target.remaining_object_dimensions;

            const auto dimension =
                static_cast<std::size_t>(
                    object.dimension_offset) +
                consumed;

            if (dimension >=
                facts.object_dimensions.size()) {
                return {status_code::configuration_failed};
            }

            capacity =
                facts.object_dimensions[dimension];

            builtin_type builtin;

            if (!graph_state->builtin(
                    target.type,
                    builtin) ||
                builtin != builtin_type::character) {
                return {status_code::configuration_failed};
            }
        }
        else {
            const auto* derived =
                graph_state->derived(target.type);

            builtin_type builtin;

            if (!derived ||
                derived->kind !=
                    derived_type_kind::array ||
                !graph_state->builtin(
                    derived->child,
                    builtin) ||
                builtin != builtin_type::character) {
                return {status_code::configuration_failed};
            }

            capacity = derived->payload;
        }

        const auto begin =
            static_cast<std::size_t>(
                literal.byte_offset);

        const auto count =
            static_cast<std::size_t>(
                literal.byte_count);

        if (capacity >
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)()) ||
            begin > facts.literal_bytes.size() ||
            count > facts.literal_bytes.size() - begin ||
            literal.byte_count > capacity) {
            return {status_code::configuration_failed};
        }

        const auto bytes =
            static_cast<std::size_t>(capacity);

        std::memset(
            target.address,
            0,
            bytes);

        if (count != 0) {
            std::memcpy(
                target.address,
                facts.literal_bytes.data() + begin,
                count);
        }

        return {};
    }

    [[nodiscard]] status apply_operation(
        const implementation_facts_storage& facts,
        source_storage& source,
        const implementation_operation_fact& operation) const noexcept {

        if (!graph_state ||
            !operation.target_path ||
            operation.target_path > facts.paths.size()) {
            return {status_code::configuration_failed};
        }

        const auto& target_fact =
            facts.paths[operation.target_path - 1];

        if (operation.kind ==
            implementation_operation_kind::binding) {
            if (!operation.source_path) {
                return {status_code::configuration_failed};
            }

            resolved_path target;
            resolved_path source_path;

            auto result =
                resolve_path(
                    facts,
                    source,
                    operation.target_path,
                    target);

            if (!result.ok() ||
                target.remaining_object_dimensions != 0) {
                return result.ok()
                    ? status{status_code::configuration_failed}
                    : result;
            }

            const auto* reference =
                graph_state->derived(target.type);

            if (!reference ||
                reference->kind !=
                    derived_type_kind::lvalue_reference) {
                return {status_code::configuration_failed};
            }

            result =
                resolve_value_path(
                    facts,
                    source,
                    operation.source_path,
                    source_path);

            if (!result.ok() ||
                source_path.type != reference->child) {
                return result.ok()
                    ? status{status_code::configuration_failed}
                    : result;
            }

            return write_reference(
                target.address,
                source_path.address);
        }

        if (operation.kind ==
            implementation_operation_kind::value_copy) {
            if (!operation.source_path) {
                return {status_code::configuration_failed};
            }

            resolved_path target;
            resolved_path source_path;

            auto result =
                resolve_value_path(
                    facts,
                    source,
                    operation.target_path,
                    target);

            if (!result.ok()) {
                return result;
            }

            result =
                resolve_value_path(
                    facts,
                    source,
                    operation.source_path,
                    source_path);

            if (!result.ok() ||
                target.type != source_path.type ||
                !copyable_value_type(target.type)) {
                return result.ok()
                    ? status{status_code::configuration_failed}
                    : result;
            }

            type_layout_record layout;

            if (!graph_state->layout(
                    target.type,
                    layout) ||
                layout.size >
                    static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)())) {
                return {status_code::configuration_failed};
            }

            std::memmove(
                target.address,
                source_path.address,
                static_cast<std::size_t>(layout.size));

            return {};
        }

        if (operation.kind !=
                implementation_operation_kind::value_literal ||
            !operation.literal ||
            operation.literal > facts.literals.size()) {
            return {status_code::configuration_failed};
        }

        const auto& literal =
            facts.literals[operation.literal - 1];

        if (literal.kind ==
            implementation_literal_kind::string) {
            return apply_string_literal(
                facts,
                source,
                target_fact,
                literal,
                operation.target_path);
        }

        resolved_path target;

        auto result =
            resolve_value_path(
                facts,
                source,
                operation.target_path,
                target);

        if (!result.ok()) {
            return result;
        }

        builtin_type builtin;

        if (!graph_state->builtin(
                target.type,
                builtin) ||
            !is_integral(builtin)) {
            return {status_code::configuration_failed};
        }

        type_layout_record layout;

        if (!graph_state->layout(
                target.type,
                layout) ||
            layout.size == 0 ||
            layout.size > sizeof(literal.bits)) {
            return {status_code::configuration_failed};
        }

        std::memcpy(
            target.address,
            &literal.bits,
            static_cast<std::size_t>(layout.size));

        return {};
    }

    [[nodiscard]] status build_source(
        const implementation_facts_storage& facts,
        source_storage& output) {

        if (!facts.source || !graph_state) {
            return {status_code::configuration_failed};
        }

        output.source = facts.source;
        output.objects.clear();
        output.bytes.units.clear();
        output.bytes.byte_size = 0;
        output.objects.reserve(facts.objects.size());

        std::uint64_t cursor = 0;

        for (const auto& object : facts.objects) {
            std::uint64_t element_count = 0;

            auto result =
                object_element_count(
                    facts,
                    object,
                    element_count);

            if (!result.ok()) {
                return result;
            }

            storage_record record;

            result =
                append_storage_record(
                    object.type,
                    element_count,
                    cursor,
                    record);

            if (!result.ok()) {
                return result;
            }

            output.objects.push_back(record);
        }

        auto result =
            output.bytes.resize_zeroed(cursor);

        if (!result.ok()) {
            return result;
        }

        for (const auto& object : output.objects) {
            type_layout_record layout;

            if (!graph_state->layout(
                    object.type,
                    layout)) {
                return {status_code::configuration_failed};
            }

            for (std::uint64_t index = 0;
                 index < object.element_count;
                 ++index) {
                std::uint64_t element_offset = 0;

                if (!checked_multiply(
                        index,
                        layout.size,
                        element_offset)) {
                    return {status_code::initialization_failed};
                }

                result =
                    construct_type(
                        object.type,
                        output.bytes.data() +
                            object.offset +
                            element_offset);

                if (!result.ok()) {
                    return result;
                }
            }
        }

        for (const auto& operation : facts.operations) {
            result =
                apply_operation(
                    facts,
                    output,
                    operation);

            if (!result.ok()) {
                return result;
            }
        }

        return {};
    }

    [[nodiscard]] status build(
        const graph& graph_value,
        std::span<const implementation_facts_storage> implementation) noexcept {

        try {
            graph_state = &graph_value;
            sources.clear();
            prepared_sources.clear();

            auto result =
                build_static_storage();

            if (!result.ok()) {
                return result;
            }

            sources.reserve(implementation.size());

            for (const auto& facts : implementation) {
                source_storage source;

                result =
                    build_source(
                        facts,
                        source);

                if (!result.ok()) {
                    return result;
                }

                sources.push_back(
                    std::move(source));
            }

            return {};
        }
        catch (...) {
            return {status_code::initialization_failed};
        }
    }

    [[nodiscard]] status prepare(
        std::span<const implementation_facts_storage> replacements,
        std::span<const std::uint32_t> source_index) noexcept {

        try {
            if (!graph_state) {
                return {status_code::invalid_state};
            }

            prepared_sources.clear();
            prepared_sources.reserve(replacements.size());

            for (const auto& facts : replacements) {
                if (!facts.source ||
                    facts.source.value() >= source_index.size()) {
                    prepared_sources.clear();
                    return {status_code::configuration_failed};
                }

                const auto slot =
                    source_index[facts.source.value()];

                if (slot > sources.size()) {
                    prepared_sources.clear();
                    return {status_code::configuration_failed};
                }

                source_storage source;

                const auto result =
                    build_source(
                        facts,
                        source);

                if (!result.ok()) {
                    prepared_sources.clear();
                    return result;
                }

                prepared_sources.push_back(
                    std::move(source));
            }

            return {};
        }
        catch (...) {
            prepared_sources.clear();
            return {status_code::initialization_failed};
        }
    }

};

runtime::runtime() = default;
runtime::~runtime() = default;

status runtime::attach(const graph& graph) noexcept {
    std::vector<implementation_facts_storage> empty;
    return attach(
        graph,
        std::move(empty));
}

status runtime::attach(
    const graph& graph,
    std::vector<implementation_facts_storage>&& implementation) noexcept {

    try {
        if (implementation.size() >
            static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return {status_code::initialization_failed};
        }

        std::uint32_t maximum_source = 0;

        for (const auto& facts : implementation) {
            if (!facts.source) {
                return {status_code::configuration_failed};
            }

            if (facts.source.value() > maximum_source) {
                maximum_source = facts.source.value();
            }
        }

        std::vector<std::uint32_t> source_index(
            static_cast<std::size_t>(maximum_source) + 1,
            0);

        for (std::size_t index = 0;
             index < implementation.size();
             ++index) {
            const auto source =
                implementation[index].source.value();

            if (source_index[source] != 0) {
                return {status_code::configuration_failed};
            }

            source_index[source] =
                static_cast<std::uint32_t>(index + 1);
        }

        auto candidate =
            std::make_unique<materialized_state>();

        auto result =
            candidate->build(
                graph,
                implementation);

        if (!result.ok()) {
            return result;
        }

        std::vector<std::uint32_t> validation_marks(
            source_index.size(),
            0);

        materialized.swap(candidate);
        implementation_source_facts.swap(implementation);
        implementation_source_index.swap(source_index);
        implementation_validation_marks.swap(validation_marks);
        implementation_validation_generation = 1;
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status runtime::prepare_implementation_replacements(
    std::span<const implementation_facts_storage> replacements) noexcept {

    if (!materialized) {
        return {status_code::invalid_state};
    }

    materialized->prepared_sources.clear();

    if (replacements.empty()) {
        return {};
    }

    try {
        std::uint32_t maximum_source = 0;
        std::size_t new_source_count = 0;

        for (const auto& facts : replacements) {
            if (!facts.source) {
                return {status_code::configuration_failed};
            }

            if (facts.source.value() > maximum_source) {
                maximum_source = facts.source.value();
            }
        }

        const auto required_index_size =
            static_cast<std::size_t>(maximum_source) + 1;

        if (implementation_source_index.size() <
                required_index_size) {
            implementation_source_index.resize(
                required_index_size,
                0);
            implementation_validation_marks.resize(
                required_index_size,
                0);
        }

        auto generation =
            ++implementation_validation_generation;

        if (generation == 0) {
            for (auto& mark : implementation_validation_marks) {
                mark = 0;
            }

            implementation_validation_generation = 1;
            generation = 1;
        }

        for (const auto& facts : replacements) {
            const auto source =
                facts.source.value();

            if (source >= implementation_source_index.size() ||
                source >= implementation_validation_marks.size() ||
                implementation_validation_marks[source] == generation) {
                return {status_code::configuration_failed};
            }

            implementation_validation_marks[source] = generation;

            const auto slot =
                implementation_source_index[source];

            if (slot > implementation_source_facts.size() ||
                slot > materialized->sources.size()) {
                return {status_code::configuration_failed};
            }

            if (slot == 0) {
                ++new_source_count;
            }
        }

        const auto maximum_slots =
            static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)());

        if (implementation_source_facts.size() > maximum_slots ||
            new_source_count >
                maximum_slots - implementation_source_facts.size()) {
            return {status_code::initialization_failed};
        }

        implementation_source_facts.reserve(
            implementation_source_facts.size() +
                new_source_count);

        materialized->sources.reserve(
            materialized->sources.size() +
                new_source_count);

        return materialized->prepare(
            replacements,
            implementation_source_index);
    }
    catch (...) {
        materialized->prepared_sources.clear();
        return {status_code::initialization_failed};
    }
}

void runtime::publish_implementation_replacements(
    std::vector<implementation_facts_storage>&& replacements) noexcept {

    assert(materialized);
    assert(
        materialized->prepared_sources.size() ==
        replacements.size());

    for (std::size_t index = 0;
         index < replacements.size();
         ++index) {
        auto& facts = replacements[index];
        auto& source =
            materialized->prepared_sources[index];

        assert(facts.source);
        assert(
            facts.source.value() <
            implementation_source_index.size());
        assert(source.source == facts.source);

        auto& slot =
            implementation_source_index[
                facts.source.value()];

        if (slot == 0) {
            assert(
                implementation_source_facts.size() <
                (std::numeric_limits<std::uint32_t>::max)());
            assert(
                implementation_source_facts.size() <
                implementation_source_facts.capacity());
            assert(
                materialized->sources.size() <
                materialized->sources.capacity());

            implementation_source_facts.push_back(
                std::move(facts));
            materialized->sources.push_back(
                std::move(source));

            slot =
                static_cast<std::uint32_t>(
                    implementation_source_facts.size());
            continue;
        }

        assert(slot <= implementation_source_facts.size());
        assert(slot <= materialized->sources.size());

        implementation_source_facts[slot - 1] =
            std::move(facts);

        materialized->sources[slot - 1] =
            std::move(source);
    }

    materialized->prepared_sources.clear();
}

status runtime::object(
    source_id source,
    std::uint32_t object_index,
    runtime_storage_view& output) const noexcept {

    output = {};

    if (!materialized ||
        !source ||
        source.value() >= implementation_source_index.size()) {
        return {status_code::not_available};
    }

    const auto slot =
        implementation_source_index[source.value()];

    if (slot == 0 ||
        slot > materialized->sources.size()) {
        return {status_code::not_available};
    }

    const auto& source_storage =
        materialized->sources[slot - 1];

    if (!object_index ||
        object_index > source_storage.objects.size()) {
        return {status_code::invalid_state};
    }

    const auto& object =
        source_storage.objects[object_index - 1];

    if (object.offset > source_storage.bytes.byte_size ||
        object.size >
            source_storage.bytes.byte_size - object.offset ||
        object.size >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return {status_code::invalid_state};
    }

    output.type = object.type;
    output.bytes = {
        source_storage.bytes.data() + object.offset,
        static_cast<std::size_t>(object.size)
    };
    output.element_count = object.element_count;
    return {};
}

status runtime::static_object(
    std::uint32_t object_index,
    runtime_storage_view& output) const noexcept {

    output = {};

    if (!materialized ||
        !object_index ||
        object_index > materialized->static_objects.size()) {
        return {status_code::not_available};
    }

    const auto& object =
        materialized->static_objects[object_index - 1];

    if (object.offset > materialized->static_bytes.byte_size ||
        object.size >
            materialized->static_bytes.byte_size - object.offset ||
        object.size >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return {status_code::invalid_state};
    }

    output.type = object.type;
    output.bytes = {
        materialized->static_bytes.data() + object.offset,
        static_cast<std::size_t>(object.size)
    };
    output.element_count = 1;
    return {};
}

} // namespace cw::server
