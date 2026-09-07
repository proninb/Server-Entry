#include "source_publisher.hpp"

#include "../builder/project_builder.hpp"
#include "../graph/graph_build_transaction.hpp"

#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <chrono>
#include <span>
#include <vector>

namespace cw::server {
namespace {

template <bool Enabled>
class publish_timing_scope final {
public:
    publish_timing_scope(
        std::uint64_t& total_ns,
        std::uint64_t& count) noexcept
        : total_ns(total_ns),
          count(count) {

        if constexpr (Enabled) {
            begin = std::chrono::steady_clock::now();
        }
    }

    publish_timing_scope(const publish_timing_scope&) = delete;
    publish_timing_scope& operator=(const publish_timing_scope&) = delete;

    ~publish_timing_scope() noexcept {
        if constexpr (Enabled) {
            total_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - begin).count());
            ++count;
        }
    }

private:
    std::uint64_t& total_ns;
    std::uint64_t& count;
    std::chrono::steady_clock::time_point begin{};
};

string_id bound_name(
    std::span<const string_id> bindings,
    source_name_ref reference) noexcept {

    return
        reference &&
        reference.index <= bindings.size()
        ? bindings[reference.index - 1]
        : string_id{};
}

status queue_name_binding(
    const source_build_entry& entry,
    source_publish_scratch& scratch,
    source_name_ref reference) noexcept {

    if (!reference ||
        reference.index >
            scratch.name_bindings.size()) {
        return {
            status_code::configuration_failed
        };
    }

    const auto slot =
        static_cast<std::size_t>(
            reference.index - 1);

    if (scratch.name_binding_seen[slot] != 0) {
        return {};
    }

    std::string_view bytes;
    std::uint64_t hash = 0;

    const auto resolved =
        entry.resolve_name_binding(
            reference,
            bytes,
            hash);

    if (!resolved.ok()) {
        return resolved;
    }

    try {
        scratch.string_bindings.push_back({
            bytes,
            hash
        });

        try {
            scratch.string_binding_slots.push_back(
                static_cast<std::uint32_t>(
                    slot));
        }
        catch (...) {
            scratch.string_bindings.pop_back();
            throw;
        }

        scratch.name_binding_seen[slot] = 1;
        return {};
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

// Validates one immutable Parser product, gathers the first-use Source-local
// spelling order, then performs one prehashed String Registry admission batch.
status prepare_source_bindings(
    graph_build_transaction& transaction,
    const source_build_entry& entry,
    source_publish_scratch& scratch) noexcept {

    try {
        scratch.name_bindings.assign(
            entry.name_count(),
            string_id{});

        scratch.name_binding_seen.assign(
            entry.name_count(),
            std::uint8_t{0});

        scratch.string_bindings.clear();
        scratch.string_binding_slots.clear();
        scratch.string_binding_results.clear();
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }

    for (const auto& fact : entry.enums) {
        if (fact.anonymous ==
                static_cast<bool>(
                    fact.canonical_name) ||
            (fact.anonymous &&
             static_cast<bool>(fact.entity)) ||
            (!fact.anonymous &&
             (!fact.entity ||
              fact.entity.source !=
                  entry.source))) {
            return {
                status_code::configuration_failed
            };
        }

        if (!fact.anonymous) {
            const auto result =
                queue_name_binding(
                    entry,
                    scratch,
                    fact.canonical_name);

            if (!result.ok()) {
                return result;
            }
        }

        if (fact.enumerator_offset >
                entry.enum_values.size() ||
            fact.enumerator_count >
                entry.enum_values.size() -
                    fact.enumerator_offset) {
            return {
                status_code::configuration_failed
            };
        }

        for (std::uint32_t index = 0;
             index < fact.enumerator_count;
             ++index) {
            const auto& value =
                entry.enum_values[
                    fact.enumerator_offset +
                    index];

            const auto result =
                queue_name_binding(
                    entry,
                    scratch,
                    value.name);

            if (!result.ok()) {
                return result;
            }
        }
    }

    for (const auto& fact : entry.aggregates) {
        if (!fact.entity ||
            fact.entity.source !=
                entry.source) {
            return {
                status_code::configuration_failed
            };
        }

        auto result =
            queue_name_binding(
                entry,
                scratch,
                fact.canonical_name);

        if (!result.ok()) {
            return result;
        }

        if (fact.member_offset >
                entry.members.size() ||
            fact.member_count >
                entry.members.size() -
                    fact.member_offset) {
            return {
                status_code::configuration_failed
            };
        }

        for (std::uint32_t index = 0;
             index < fact.member_count;
             ++index) {
            const auto& member =
                entry.members[
                    fact.member_offset + index];

            if (!member.name ||
                (member.builtin &&
                 member.type_entity) ||
                (!member.builtin &&
                 !member.type_entity)) {
                return {
                    status_code::configuration_failed
                };
            }

            result =
                queue_name_binding(
                    entry,
                    scratch,
                    member.name);

            if (!result.ok()) {
                return result;
            }

            if (member.modifier_offset >
                    entry.modifiers.size() ||
                member.modifier_count >
                    entry.modifiers.size() -
                        member.modifier_offset) {
                return {
                    status_code::configuration_failed
                };
            }

            for (std::uint32_t modifier_index = 0;
                 modifier_index <
                    member.modifier_count;
                 ++modifier_index) {
                switch (entry.modifiers[
                            member.modifier_offset +
                            modifier_index].kind) {
                case source_type_modifier_kind::pointer:
                case source_type_modifier_kind::array:
                case source_type_modifier_kind::lvalue_reference:
                case source_type_modifier_kind::rvalue_reference:
                    break;
                default:
                    return {
                        status_code::configuration_failed
                    };
                }
            }
        }
    }

    try {
        scratch.string_binding_results.resize(
            scratch.string_bindings.size());
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }

    auto result =
        transaction.strings().bind_prehashed(
            scratch.string_bindings,
            scratch.string_binding_results);

    if (!result.ok()) {
        return result;
    }

    if (scratch.string_binding_slots.size() !=
        scratch.string_binding_results.size()) {
        return {
            status_code::initialization_failed
        };
    }

    for (std::size_t index = 0;
         index <
            scratch.string_binding_results.size();
         ++index) {
        const auto slot =
            scratch.string_binding_slots[index];

        const auto canonical =
            scratch.string_binding_results[index];

        if (slot >=
                scratch.name_bindings.size() ||
            !canonical) {
            return {
                status_code::initialization_failed
            };
        }

        scratch.name_bindings[slot] =
            canonical;
    }

    return {};
}

} // namespace
template <bool Detailed>
status publish_source_entry_impl(
    graph_build_transaction& transaction,
    const source_build_entry& entry,
    const project_builder& builder,
    const operation_id operation,
    diagnostic_buffer& diagnostics,
    source_publish_scratch& scratch) noexcept {

    const auto abort = [](status result) noexcept {
        return result;
    };

    const auto emit = [&](
        const diagnostic_descriptor& descriptor,
        source_range location) noexcept {
        try {
            diagnostics.emit({
                descriptor.id,
                descriptor.default_severity,
                operation,
                location,
                {}
            });
            return status{};
        }
        catch (...) {
            return status{status_code::initialization_failed};
        }
    };

    const auto malformed = [&]() noexcept {
        const auto emitted = emit(
            diagnostics::builder_invalid_source_fact,
            {entry.source, 0, 0});
        return abort(emitted.ok()
            ? status{status_code::configuration_failed}
            : emitted);
    };

    const auto infrastructure = [&]() noexcept {
        const auto emitted = emit(
            diagnostics::construction_initialization_failed,
            {entry.source, 0, 0});
        return abort(emitted.ok()
            ? status{status_code::initialization_failed}
            : emitted);
    };

    if (!entry.source) {
        return malformed();
    }

    try {
        const auto binding_result =
            prepare_source_bindings(
                transaction,
                entry,
                scratch);

        if (!binding_result.ok()) {
            return
                binding_result.code ==
                    status_code::initialization_failed
                ? infrastructure()
                : malformed();
        }

        // RC21 identity boundary: no text-to-string_id work is permitted below.
        graph_update::source_replacement replacement;
        status result;

        {
            publish_timing_scope<Detailed> timing{
                scratch.telemetry.source_replace_ns,
                scratch.telemetry.source_count
            };

            result = transaction.graph_state().replace_source(
                entry.source,
                replacement);
        }

        if (!result.ok()) {
            if (result.code == status_code::duplicate_source_replacement) {
                const auto emitted = emit(
                    diagnostics::builder_duplicate_source_replacement,
                    {entry.source, 0, 0});
                return abort(emitted.ok() ? result : emitted);
            }

            return result.code == status_code::initialization_failed
                ? infrastructure()
                : abort(result);
        }

        std::size_t named_count =
            entry.aggregates.size();
        std::size_t anonymous_count = 0;

        for (const auto& fact : entry.enums) {
            if (fact.anonymous) {
                ++anonymous_count;
            }
            else {
                ++named_count;
            }
        }

        result =
            replacement.reserve(
                named_count,
                anonymous_count,
                entry.enum_values.size());

        if (!result.ok()) {
            return result.code ==
                    status_code::initialization_failed
                ? infrastructure()
                : abort(result);
        }
        auto& enum_values = scratch.enum_values;
        auto& members = scratch.members;
        auto& modifiers = scratch.modifiers;

        enum_values.clear();
        members.clear();
        modifiers.clear();

        scratch.builder.detailed = Detailed;

        std::uint32_t enum_prepare_ordinal = 0;

        for (const auto& fact : entry.enums) {
            const bool sample =
                [&]() noexcept {
                    if constexpr (Detailed) {
                        ++enum_prepare_ordinal;
                        return
                            (enum_prepare_ordinal % 1024u) == 0;
                    }

                    return false;
                }();

            string_id canonical_name;

            std::chrono::steady_clock::time_point name_begin{};

            if constexpr (Detailed) {
                if (sample) {
                    name_begin =
                        std::chrono::steady_clock::now();
                }
            }

            if (!fact.anonymous) {
                canonical_name =
                    bound_name(scratch.name_bindings,
                        fact.canonical_name);

                if (!canonical_name) {
                    return malformed();
                }
            }

            if constexpr (Detailed) {
                if (sample) {
                    scratch.telemetry.enum_name_ns +=
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() -
                                    name_begin).count());
                }
            }

            std::chrono::steady_clock::time_point values_begin{};

            if constexpr (Detailed) {
                if (sample) {
                    values_begin =
                        std::chrono::steady_clock::now();
                }
            }

            if (fact.enumerator_offset > entry.enum_values.size() ||
                fact.enumerator_count >
                    entry.enum_values.size() -
                    fact.enumerator_offset) {
                return malformed();
            }

            enum_values.clear();
            enum_values.reserve(fact.enumerator_count);

            for (std::uint32_t index = 0;
                 index < fact.enumerator_count;
                 ++index) {
                const auto& value =
                    entry.enum_values[
                        fact.enumerator_offset + index];

                const auto name =
                    bound_name(scratch.name_bindings,
                        value.name);

                if (!name) {
                    return malformed();
                }

                enum_values.push_back({
                    name,
                    value.value
                });
            }

            if constexpr (Detailed) {
                if (sample) {
                    scratch.telemetry.enum_values_ns +=
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() -
                                    values_begin).count());
                }
            }

            const enum_source_fact canonical{
                canonical_name,
                fact.anonymous,
                fact.scoped,
                fact.definition_state,
                fact.explicit_underlying,
                enum_values,
                fact.entity
            };

            {
                publish_timing_scope<Detailed> timing{
                    scratch.telemetry.enum_builder_ns,
                    scratch.telemetry.enum_builder_count
                };

                result = builder.build_enum(
                    replacement,
                    canonical,
                    scratch.builder);
            }

            if (!result.ok()) {
                if (result.code == status_code::configuration_failed) {
                    const auto emitted = emit(
                        diagnostics::builder_semantic_failure,
                        {entry.source,
                         fact.declaration_range.offset,
                         fact.declaration_range.length});
                    return abort(emitted.ok() ? result : emitted);
                }
                return result.code == status_code::initialization_failed
                    ? infrastructure()
                    : abort(result);
            }
        }

        for (const auto& fact : entry.aggregates) {
            const auto canonical_name =
                bound_name(scratch.name_bindings,
                    fact.canonical_name);

            if (!canonical_name) {
                return malformed();
            }

            if (fact.member_offset > entry.members.size() ||
                fact.member_count > entry.members.size() - fact.member_offset) {
                return malformed();
            }

            members.clear();
            modifiers.clear();
            members.reserve(fact.member_count);

            for (std::uint32_t index = 0; index < fact.member_count; ++index) {
                const auto& member = entry.members[fact.member_offset + index];
                const auto member_name =
                    bound_name(scratch.name_bindings,
                        member.name);

                if (!member_name) {
                    return malformed();
                }

                string_id user_type_name;

                if (!member.builtin &&
                    !member.type_entity) {
                    return malformed();
                }

                if (member.modifier_offset > entry.modifiers.size() ||
                    member.modifier_count > entry.modifiers.size() - member.modifier_offset) {
                    return malformed();
                }

                const auto modifier_offset =
                    static_cast<std::uint32_t>(modifiers.size());

                for (std::uint32_t modifier_index = 0;
                     modifier_index < member.modifier_count;
                     ++modifier_index) {
                    const auto& modifier =
                        entry.modifiers[
                            member.modifier_offset +
                            modifier_index];

                    derived_type_kind kind;

                    switch (modifier.kind) {
                    case source_type_modifier_kind::pointer:
                        kind = derived_type_kind::pointer;
                        break;
                    case source_type_modifier_kind::array:
                        kind = derived_type_kind::array;
                        break;
                    case source_type_modifier_kind::lvalue_reference:
                        kind = derived_type_kind::lvalue_reference;
                        break;
                    case source_type_modifier_kind::rvalue_reference:
                        kind = derived_type_kind::rvalue_reference;
                        break;
                    default:
                        return malformed();
                    }

                    modifiers.push_back({
                        kind,
                        modifier.payload
                    });
                }

                members.push_back({
                    member_name,
                    member.builtin,
                    user_type_name,
                    modifier_offset,
                    member.modifier_count,
                    member.type_entity
                });
            }

            {
                publish_timing_scope<Detailed> timing{
                    scratch.telemetry.aggregate_builder_ns,
                    scratch.telemetry.aggregate_builder_count
                };

                result = builder.build_aggregate(
                    replacement,
                    {
                        canonical_name,
                        fact.definition_state,
                        members,
                        modifiers,
                        fact.entity
                    },
                    scratch.builder);
            }

            if (!result.ok()) {
                if (result.code == status_code::configuration_failed) {
                    const auto emitted = emit(
                        diagnostics::builder_semantic_failure,
                        {entry.source,
                         fact.declaration_range.offset,
                         fact.declaration_range.length});
                    return abort(emitted.ok() ? result : emitted);
                }
                return result.code == status_code::initialization_failed
                    ? infrastructure()
                    : abort(result);
            }
        }

        return {};
    }
    catch (...) {
        return infrastructure();
    }
}

status publish_source_entry(
    graph_build_transaction& transaction,
    const source_build_entry& entry,
    const project_builder& builder,
    const operation_id operation,
    diagnostic_buffer& diagnostics,
    source_publish_scratch& scratch) noexcept {

    const auto result =
        scratch.telemetry.enabled
            ? publish_source_entry_impl<true>(
                  transaction,
                  entry,
                  builder,
                  operation,
                  diagnostics,
                  scratch)
            : publish_source_entry_impl<false>(
                  transaction,
                  entry,
                  builder,
                  operation,
                  diagnostics,
                  scratch);

    if (!result.ok()) {
        // The friend boundary owns fail-closed transaction cancellation.
        transaction.fail(result);
    }

    return result;
}

status publish_source_entry(
    graph_build_transaction& transaction,
    const source_build_entry& entry,
    const project_builder& builder,
    const operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    source_publish_scratch scratch;

    return publish_source_entry(
        transaction,
        entry,
        builder,
        operation,
        diagnostics,
        scratch);
}

} // namespace cw::server
