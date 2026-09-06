#include "project_builder.hpp"

#include "../../diagnostics/diagnostic_descriptor.hpp"
#include "../graph/graph_build_transaction.hpp"

#include <chrono>
#include <limits>
#include <new>
#include <vector>

namespace cw::server {
namespace {

std::uint64_t builder_elapsed_ns(
    std::chrono::steady_clock::time_point begin) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count());
}

template <bool Detailed>
status build_enum_impl(
    graph_update::source_replacement& replacement,
    const enum_source_fact& fact,
    project_builder_scratch& scratch) noexcept {

    try {
        const enum_build_data data{
            fact.definition_state,
            fact.scoped,
            fact.explicit_underlying,
            fact.enumerators
        };

        type_handle type;
        status result;

        std::chrono::steady_clock::time_point mutation_begin{};

        if constexpr (Detailed) {
            mutation_begin = std::chrono::steady_clock::now();
        }

        if (fact.anonymous) {
            result =
                replacement.add_anonymous_enum(
                    data,
                    type);
        }
        else {
            stable_id entity;

            if constexpr (Detailed) {
                result =
                    replacement.add_named_enum(
                        fact.canonical_name,
                        data,
                        entity,
                        type,
                        &scratch.named_enum);
            }
            else {
                result =
                    replacement.add_named_enum(
                        fact.canonical_name,
                        data,
                        entity,
                        type);
            }

            if (result.ok() && !entity) {
                return {
                    status_code::initialization_failed
                };
            }
        }

        if constexpr (Detailed) {
            scratch.enum_graph_mutation_ns +=
                builder_elapsed_ns(mutation_begin);
        }

        if (result.ok() && !type) {
            return {
                status_code::initialization_failed
            };
        }

        return result;
    }
    catch (...) {
        return {
            status_code::initialization_failed
        };
    }
}

status build_aggregate_impl(
    graph_update::source_replacement& replacement,
    const aggregate_source_fact& fact,
    project_builder_scratch& scratch) noexcept {

    stable_id entity;
    type_handle type;

    auto result =
        replacement.add_named_type(
            fact.canonical_name,
            fact.definition_state,
            entity,
            type);

    if (!result.ok()) {
        return result;
    }

    if (!entity || !type) {
        return {status_code::initialization_failed};
    }

    if (fact.definition_state !=
        aggregate_definition_state::defined) {
        return fact.members.empty()
            ? status{}
            : status{status_code::configuration_failed};
    }

    try {
        auto& members = scratch.members;
        auto& modifiers = scratch.modifiers;

        members.clear();
        modifiers.clear();

        members.reserve(fact.members.size());
        modifiers.reserve(fact.modifiers.size());

        for (const auto& modifier : fact.modifiers) {
            modifiers.push_back({
                modifier.kind,
                modifier.payload
            });
        }

        for (const auto& member : fact.members) {
            if (!member.name ||
                (member.builtin.has_value() ==
                 static_cast<bool>(member.user_type_name)) ||
                member.modifier_offset > modifiers.size() ||
                member.modifier_count >
                    modifiers.size() - member.modifier_offset) {
                return {status_code::configuration_failed};
            }

            members.push_back({
                member.name,
                member.builtin,
                member.user_type_name,
                member.modifier_offset,
                member.modifier_count
            });
        }

        // Named member bases are allowed to remain canonically pending here.
        // Graph resolves them only after all Source publications have completed.
        return replacement.define_members(
            type,
            members,
            modifiers);
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status build_aggregate_impl(
    graph_update::source_replacement& replacement,
    const aggregate_source_fact& fact) noexcept {

    project_builder_scratch scratch;

    return build_aggregate_impl(
        replacement,
        fact,
        scratch);
}

} // namespace

status project_builder::build(
    graph_build_transaction& transaction,
    const std::span<const source_fact_batch> sources,
    const operation_id operation,
    diagnostic_buffer& diagnostics) const noexcept {

    const auto abort =
        [&transaction](status result) noexcept {
            transaction.fail(result);
            return result;
        };

    try {
        for (const auto& batch : sources) {
            if (!batch.source) {
                return abort({
                    status_code::configuration_failed
                });
            }

            for (const auto& fact : batch.enums) {
                if (fact.anonymous ==
                    static_cast<bool>(
                        fact.canonical_name)) {
                    return abort({
                        status_code::configuration_failed
                    });
                }
            }

            for (const auto& fact : batch.aggregates) {
                if (!fact.canonical_name) {
                    return abort({
                        status_code::configuration_failed
                    });
                }
            }
        }


        for (const auto& batch : sources) {
            graph_update::source_replacement replacement;

            const auto opened =
                transaction.graph_state().replace_source(
                    batch.source,
                    replacement);

            if (!opened.ok()) {
                if (opened.code ==
                    status_code::duplicate_source_replacement) {
                    try {
                        diagnostics.emit({
                            diagnostics::builder_duplicate_source_replacement.id,
                            diagnostics::builder_duplicate_source_replacement.default_severity,
                            operation,
                            {batch.source, 0, 0},
                            {}
                        });
                    }
                    catch (...) {
                        return abort({
                            status_code::initialization_failed
                        });
                    }
                }

                return abort(opened);
            }

            std::size_t named_count =
                batch.aggregates.size();
            std::size_t anonymous_count = 0;
            std::size_t enum_value_count = 0;

            for (const auto& fact : batch.enums) {
                if (fact.anonymous) {
                    ++anonymous_count;
                }
                else {
                    ++named_count;
                }

                if (fact.enumerators.size() >
                    (std::numeric_limits<std::size_t>::max)() -
                        enum_value_count) {
                    return abort({
                        status_code::initialization_failed
                    });
                }

                enum_value_count +=
                    fact.enumerators.size();
            }

            const auto reserved =
                replacement.reserve(
                    named_count,
                    anonymous_count,
                    enum_value_count);

            if (!reserved.ok()) {
                return abort(reserved);
            }

            for (const auto& fact : batch.enums) {
                const enum_build_data data{
                    fact.definition_state,
                    fact.scoped,
                    fact.explicit_underlying,
                    fact.enumerators
                };

                type_handle type{};
                status result{};

                if (fact.anonymous) {
                    result =
                        replacement.add_anonymous_enum(
                            data,
                            type);
                }
                else {
                    stable_id entity{};

                    result =
                        replacement.add_named_enum(
                            fact.canonical_name,
                            data,
                            entity,
                            type);

                    if (result.ok() &&
                        (!entity || !type)) {
                        return abort({
                            status_code::initialization_failed
                        });
                    }
                }

                if (!result.ok()) {
                    return abort(result);
                }

                if (!type) {
                    return abort({
                        status_code::initialization_failed
                    });
                }
            }

            for (const auto& fact :
                 batch.aggregates) {
                const auto result =
                    build_aggregate_impl(
                        replacement,
                        fact);

                if (!result.ok()) {
                    return abort(result);
                }
            }
        }

        return {};
    }
    catch (const std::bad_alloc&) {
        return abort({
            status_code::initialization_failed
        });
    }
    catch (...) {
        return abort({
            status_code::initialization_failed
        });
    }
}

status project_builder::build_enum(
    graph_update::source_replacement& replacement,
    const enum_source_fact& fact) const noexcept {

    project_builder_scratch scratch;

    return build_enum(
        replacement,
        fact,
        scratch);
}

status project_builder::build_enum(
    graph_update::source_replacement& replacement,
    const enum_source_fact& fact,
    project_builder_scratch& scratch) const noexcept {

    return scratch.detailed
        ? build_enum_impl<true>(
              replacement,
              fact,
              scratch)
        : build_enum_impl<false>(
              replacement,
              fact,
              scratch);
}

status project_builder::build_aggregate(
    graph_update::source_replacement& replacement,
    const aggregate_source_fact& fact) const noexcept {

    return build_aggregate_impl(
        replacement,
        fact);
}

status project_builder::build_aggregate(
    graph_update::source_replacement& replacement,
    const aggregate_source_fact& fact,
    project_builder_scratch& scratch) const noexcept {

    return build_aggregate_impl(
        replacement,
        fact,
        scratch);
}

} // namespace cw::server
