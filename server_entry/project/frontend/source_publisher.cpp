#include "source_publisher.hpp"

#include "../builder/project_builder.hpp"
#include "../graph/graph_build_transaction.hpp"
#include "../parser/source_context.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <chrono>
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

status copy_name(
    const source_context& context,
    source_name_ref source,
    source_build_entry& destination,
    build_name_ref& output) noexcept {

    output = {};

    if (!source) {
        return {};
    }

    std::string_view bytes;
    auto result = context.resolve_name(source, bytes);

    if (!result.ok()) {
        return result;
    }

    return destination.store_name(bytes, output);
}

// String Binding is the only text-to-project-ID boundary. It runs before Graph
// mutation and binds every captured name exactly once. Publication below this
// function is ID-only.
status bind_source_names(
    graph_build_transaction& transaction,
    source_build_entry& entry) noexcept {

    for (std::uint32_t raw = 1;
         raw <= entry.name_count();
         ++raw) {
        const build_name_ref reference{
            raw
        };

        std::string_view bytes;

        auto result =
            entry.resolve_name(
                reference,
                bytes);

        if (!result.ok()) {
            return result;
        }

        string_id canonical;

        result =
            transaction.strings().bind(
                bytes,
                canonical);

        if (!result.ok() ||
            !canonical) {
            return result.ok()
                ? status{
                    status_code::
                        configuration_failed}
                : result;
        }

        result =
            entry.bind_name(
                reference,
                canonical);

        if (!result.ok()) {
            return result;
        }
    }

    return {};
}
} // namespace

status capture_source_facts(
    const parser_source_fact_batch& batch,
    source_build_entry& output) noexcept {

    output.reset();

    if (!batch.source || !batch.context) {
        return {status_code::configuration_failed};
    }

    try {
        output.source = batch.source;

        output.enums.reserve(batch.enums.size());
        output.aggregates.reserve(batch.aggregates.size());

        for (const auto& fact : batch.enums) {
            if (fact.anonymous == static_cast<bool>(fact.canonical_name)) {
                return {status_code::configuration_failed};
            }

            source_build_enum captured;
            captured.anonymous = fact.anonymous;
            captured.scoped = fact.scoped;
            captured.definition_state = fact.definition_state;
            captured.explicit_underlying = fact.explicit_underlying;
            captured.declaration_range = fact.declaration_range;
            captured.name_range = fact.name_range;

            auto result = copy_name(
                *batch.context,
                fact.canonical_name,
                output,
                captured.canonical_name);

            if (!result.ok()) {
                return result;
            }

            const auto values = batch.context->enumerators(fact);

            if (values.size() != fact.enumerator_count) {
                return {status_code::configuration_failed};
            }

            captured.value_offset = static_cast<std::uint32_t>(output.enum_values.size());
            captured.value_count = static_cast<std::uint32_t>(values.size());

            for (const auto& value : values) {
                source_build_enum_value captured_value;
                captured_value.value = value.value;
                captured_value.name_range = value.name_range;

                result = copy_name(
                    *batch.context,
                    value.name,
                    output,
                    captured_value.name);

                if (!result.ok() || !captured_value.name) {
                    return result.ok()
                        ? status{status_code::configuration_failed}
                        : result;
                }

                output.enum_values.push_back(captured_value);
            }

            output.enums.push_back(captured);
        }

        for (const auto& fact : batch.aggregates) {
            source_build_aggregate captured;
            captured.definition_state = fact.definition_state;
            captured.declaration_range = fact.declaration_range;
            captured.name_range = fact.name_range;

            auto result = copy_name(
                *batch.context,
                fact.canonical_name,
                output,
                captured.canonical_name);

            if (!result.ok() || !captured.canonical_name) {
                return result.ok()
                    ? status{status_code::configuration_failed}
                    : result;
            }

            const auto source_members = batch.context->members(fact);

            if (source_members.size() != fact.member_count) {
                return {status_code::configuration_failed};
            }

            captured.member_offset = static_cast<std::uint32_t>(output.members.size());
            captured.member_count = static_cast<std::uint32_t>(source_members.size());

            for (const auto& member : source_members) {
                source_build_member captured_member;
                captured_member.builtin = member.builtin;

                result = copy_name(
                    *batch.context,
                    member.name,
                    output,
                    captured_member.name);

                if (!result.ok() || !captured_member.name) {
                    return result.ok()
                        ? status{status_code::configuration_failed}
                        : result;
                }

                if (!member.builtin) {
                    result = copy_name(
                        *batch.context,
                        member.type_name,
                        output,
                        captured_member.user_type_name);

                    if (!result.ok() || !captured_member.user_type_name) {
                        return result.ok()
                            ? status{status_code::configuration_failed}
                            : result;
                    }
                }

                const auto source_modifiers = batch.context->modifiers(member);

                if (source_modifiers.size() != member.modifier_count) {
                    return {status_code::configuration_failed};
                }

                captured_member.modifier_offset =
                    static_cast<std::uint32_t>(output.modifiers.size());
                captured_member.modifier_count =
                    static_cast<std::uint32_t>(source_modifiers.size());

                for (const auto& modifier : source_modifiers) {
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
                        return {status_code::configuration_failed};
                    }

                    output.modifiers.push_back({kind, modifier.payload});
                }

                output.members.push_back(captured_member);
            }

            output.aggregates.push_back(captured);
        }

        return {};
    }
    catch (...) {
        output.reset();
        return {status_code::initialization_failed};
    }
}

template <bool Detailed>
status publish_source_entry_impl(
    graph_build_transaction& transaction,
    source_build_entry& entry,
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
            bind_source_names(
                transaction,
                entry);

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
                    entry.bound_name(
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

            if (fact.value_offset > entry.enum_values.size() ||
                fact.value_count >
                    entry.enum_values.size() -
                    fact.value_offset) {
                return malformed();
            }

            enum_values.clear();
            enum_values.reserve(fact.value_count);

            for (std::uint32_t index = 0;
                 index < fact.value_count;
                 ++index) {
                const auto& value =
                    entry.enum_values[
                        fact.value_offset + index];

                const auto name =
                    entry.bound_name(
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
                enum_values
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
                entry.bound_name(
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
                    entry.bound_name(
                        member.name);

                if (!member_name) {
                    return malformed();
                }

                string_id user_type_name;

                if (!member.builtin) {
                    user_type_name =
                        entry.bound_name(
                            member.user_type_name);

                    if (!user_type_name) {
                        return malformed();
                    }
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
                        entry.modifiers[member.modifier_offset + modifier_index];
                    modifiers.push_back({modifier.kind, modifier.payload});
                }

                members.push_back({
                    member_name,
                    member.builtin,
                    user_type_name,
                    modifier_offset,
                    member.modifier_count
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
                        modifiers
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
    source_build_entry& entry,
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
    source_build_entry& entry,
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

status publish_source_facts(
    graph_build_transaction& transaction,
    const parser_source_fact_batch& batch,
    const project_builder& builder,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    source_build_entry entry;
    auto result = capture_source_facts(batch, entry);

    if (!result.ok()) {
        try {
            const auto& descriptor =
                result.code == status_code::configuration_failed
                    ? diagnostics::builder_invalid_source_fact
                    : diagnostics::construction_initialization_failed;

            diagnostics.emit({
                descriptor.id,
                descriptor.default_severity,
                operation,
                {batch.source, 0, 0},
                {}
            });
        }
        catch (...) {
            result = {status_code::initialization_failed};
        }

        transaction.fail(result);
        return result;
    }

    return publish_source_entry(
        transaction,
        entry,
        builder,
        operation,
        diagnostics);
}

} // namespace cw::server
