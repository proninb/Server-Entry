#include "../server_entry/project/builder/project_builder.hpp"
#include "../server_entry/project/frontend/source_publisher.hpp"
#include "../server_entry/project/graph/graph_build_transaction.hpp"
#include "../server_entry/project/graph/graph_manager.hpp"
#include "../server_entry/project/parser/source_context.hpp"
#include "../server_entry/diagnostics/diagnostic_descriptor.hpp"

#include <array>
#include <concepts>
#include <filesystem>
#include <iostream>
#include <span>
#include <type_traits>

namespace cw::server {

class graph_build_transaction_test_access {
public:
    static std::size_t contribution_count(
        const graph_manager& value,
        source_id source) noexcept {

        return value.source_contribution_cache_state.contribution_count(source);
    }
};

} // namespace cw::server

namespace {

using namespace cw::server;
using access = graph_build_transaction_test_access;

template <typename T>
concept has_entity_id_field = requires(T value) {
    value.id;
};

template <typename T>
concept has_defining_source_field = requires(T value) {
    value.defining_source;
};

template <typename T>
concept has_aggregate_payload_field = requires(T value) {
    value.aggregate;
};

template <typename T>
concept has_graph_contribution_count = requires(const T& value) {
    value.contribution_count(source_id{1});
};

static_assert(!has_entity_id_field<entity_entry>);
static_assert(!has_defining_source_field<entity_entry>);
static_assert(!has_aggregate_payload_field<type_entry>);
static_assert(!has_graph_contribution_count<graph>);

const std::filesystem::path source_a = LR"(C:\builder\a.cpp)";
const std::filesystem::path source_b = LR"(C:\builder\b.cpp)";

bool resolve_source(
    graph_build_transaction& transaction,
    const std::filesystem::path& path,
    source_id& output) {

    return transaction.sources().resolve(
        path,
        project_item_role::source,
        output).ok();
}

bool intern(
    graph_build_transaction& transaction,
    std::string_view text,
    string_id& output) {

    return transaction.strings().intern(
        text,
        output).ok();
}

bool build_batches(
    project_builder& builder,
    graph_build_transaction& transaction,
    std::span<const source_fact_batch> batches,
    diagnostic_buffer& diagnostics,
    std::uint64_t operation = 1) {

    return builder.build(
        transaction,
        batches,
        operation_id{operation},
        diagnostics).ok();
}

bool test_named_enum_materialization() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;
    string_id enum_name;
    string_id value_name;

    if (!resolve_source(transaction, source_a, source) ||
        !intern(transaction, "N::Mode", enum_name) ||
        !intern(transaction, "Ready", value_name)) {
        return false;
    }

    const std::array values{
        enum_value_fact{
            value_name,
            {builtin_type::integer, 7}
        }
    };

    const std::array enums{
        enum_source_fact{
            enum_name,
            false,
            true,
            enum_definition_state::defined,
            builtin_type::integer,
            values
        }
    };

    const std::array batches{
        source_fact_batch{
            source,
            enums,
            {}
        }
    };

    if (!build_batches(
            builder,
            transaction,
            batches,
            diagnostics) ||
        !transaction.commit().ok()) {
        return false;
    }

    const auto identity =
        manager.compiled_graph().find_id(enum_name);

    const auto* entity =
        manager.compiled_graph().find(identity);

    const auto* type =
        entity
            ? manager.compiled_graph().find(entity->type)
            : nullptr;

    const auto materialized =
        entity
            ? manager.compiled_graph().enum_values(entity->type)
            : std::span<const enum_value_record>{};

    return
        identity &&
        entity &&
        entity->kind == entity_kind::enum_type &&
        type &&
        type->kind == user_type_kind::enumeration &&
        type->enumeration.scoped &&
        type->enumeration.fixed_underlying &&
        type->enumeration.underlying == builtin_type::integer &&
        type->definition &&
        materialized.size() == 1 &&
        materialized[0].name == value_name &&
        materialized[0].bits == 7 &&
        access::contribution_count(manager, source) == 1;
}

bool test_duplicate_source_diagnostic_is_fail_closed() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;
    string_id first_name;
    string_id second_name;

    if (!resolve_source(transaction, source_a, source) ||
        !intern(transaction, "First", first_name) ||
        !intern(transaction, "Second", second_name)) {
        return false;
    }

    const std::array first_enums{
        enum_source_fact{
            first_name,
            false,
            false,
            enum_definition_state::opaque,
            builtin_type::integer,
            {}
        }
    };

    const std::array second_enums{
        enum_source_fact{
            second_name,
            false,
            false,
            enum_definition_state::opaque,
            builtin_type::integer,
            {}
        }
    };

    const std::array batches{
        source_fact_batch{source, first_enums, {}},
        source_fact_batch{source, second_enums, {}}
    };

    const auto result =
        builder.build(
            transaction,
            batches,
            operation_id{2},
            diagnostics);

    if (result.ok() ||
        result.code != status_code::duplicate_source_replacement ||
        diagnostics.empty() ||
        transaction.commit().ok()) {
        return false;
    }

    bool found = false;

    for (const auto& record : diagnostics.records()) {
        if (record.id ==
            diagnostics::builder_duplicate_source_replacement.id) {
            found = true;
            break;
        }
    }

    return
        found &&
        manager.state() == project_state::error &&
        manager.compiled_graph().entity_count() == 0;
}

bool test_invalid_builder_fact_shape() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    const std::array batches{
        source_fact_batch{}
    };

    const auto result =
        builder.build(
            transaction,
            batches,
            operation_id{3},
            diagnostics);

    return
        !result.ok() &&
        result.code == status_code::configuration_failed &&
        !transaction.commit().ok() &&
        manager.state() == project_state::error &&
        manager.compiled_graph().entity_count() == 0;
}

bool test_multi_source_definition_and_removal() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    source_id declaration_source;
    source_id definition_source;
    string_id name;
    string_id value_name;
    stable_id identity;
    type_handle type;

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        if (!resolve_source(
                transaction,
                source_a,
                declaration_source) ||
            !resolve_source(
                transaction,
                source_b,
                definition_source) ||
            !intern(transaction, "Shared", name) ||
            !intern(transaction, "Value", value_name)) {
            return false;
        }

        const std::array defined_values{
            enum_value_fact{
                value_name,
                {builtin_type::integer, 11}
            }
        };

        const std::array declaration{
            enum_source_fact{
                name,
                false,
                false,
                enum_definition_state::opaque,
                builtin_type::integer,
                {}
            }
        };

        const std::array definition{
            enum_source_fact{
                name,
                false,
                false,
                enum_definition_state::defined,
                builtin_type::integer,
                defined_values
            }
        };

        const std::array batches{
            source_fact_batch{
                declaration_source,
                declaration,
                {}
            },
            source_fact_batch{
                definition_source,
                definition,
                {}
            }
        };

        if (!build_batches(
                builder,
                transaction,
                batches,
                diagnostics,
                4) ||
            !transaction.commit().ok()) {
            return false;
        }

        identity =
            manager.compiled_graph().find_id(name);

        const auto* entity =
            manager.compiled_graph().find(identity);

        if (!entity) {
            return false;
        }

        type = entity->type;

        if (manager.compiled_graph().enum_values(type).size() != 1 ||
            access::contribution_count(
                manager,
                declaration_source) != 1 ||
            access::contribution_count(
                manager,
                definition_source) != 1) {
            return false;
        }
    }

    diagnostics.clear();

    {
        auto transaction =
            manager.begin_build(graph_build_mode::incremental);

        source_id same_source;

        if (!resolve_source(
                transaction,
                source_b,
                same_source) ||
            same_source != definition_source) {
            return false;
        }

        const std::array batches{
            source_fact_batch{
                same_source,
                {},
                {}
            }
        };

        if (!build_batches(
                builder,
                transaction,
                batches,
                diagnostics,
                5) ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    const auto* remaining =
        manager.compiled_graph().find(identity);

    const auto* remaining_type =
        remaining
            ? manager.compiled_graph().find(
                  remaining->type)
            : nullptr;

    if (!remaining ||
        remaining->type != type ||
        !remaining_type ||
        remaining_type->definition ||
        !manager.compiled_graph().enum_values(type).empty() ||
        access::contribution_count(
            manager,
            declaration_source) != 1 ||
        access::contribution_count(
            manager,
            definition_source) != 0) {
        return false;
    }

    diagnostics.clear();

    {
        auto transaction =
            manager.begin_build(graph_build_mode::incremental);

        source_id same_source;

        if (!resolve_source(
                transaction,
                source_a,
                same_source) ||
            same_source != declaration_source) {
            return false;
        }

        const std::array batches{
            source_fact_batch{
                same_source,
                {},
                {}
            }
        };

        if (!build_batches(
                builder,
                transaction,
                batches,
                diagnostics,
                6) ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    return
        manager.compiled_graph().find(identity) == nullptr &&
        manager.compiled_graph().find_id(name) == identity &&
        access::contribution_count(
            manager,
            declaration_source) == 0;
}

bool test_identity_resurrection_through_builder() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    source_id source;
    string_id name;
    stable_id original;

    {
        auto transaction =
            manager.begin_build(graph_build_mode::rebuild);

        if (!resolve_source(transaction, source_a, source) ||
            !intern(transaction, "Resurrect", name)) {
            return false;
        }

        const std::array facts{
            enum_source_fact{
                name,
                false,
                false,
                enum_definition_state::opaque,
                builtin_type::integer,
                {}
            }
        };

        const std::array batches{
            source_fact_batch{
                source,
                facts,
                {}
            }
        };

        if (!build_batches(
                builder,
                transaction,
                batches,
                diagnostics,
                7) ||
            !transaction.commit().ok()) {
            return false;
        }

        original =
            manager.compiled_graph().find_id(name);
    }

    diagnostics.clear();

    {
        auto transaction =
            manager.begin_build(graph_build_mode::incremental);

        source_id same_source;

        if (!resolve_source(
                transaction,
                source_a,
                same_source)) {
            return false;
        }

        const std::array batches{
            source_fact_batch{
                same_source,
                {},
                {}
            }
        };

        if (!build_batches(
                builder,
                transaction,
                batches,
                diagnostics,
                8) ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    if (manager.compiled_graph().find(original) != nullptr ||
        manager.compiled_graph().find_id(name) != original) {
        return false;
    }

    diagnostics.clear();

    {
        auto transaction =
            manager.begin_build(graph_build_mode::incremental);

        source_id same_source;
        string_id same_name;

        if (!resolve_source(
                transaction,
                source_a,
                same_source) ||
            !intern(
                transaction,
                "Resurrect",
                same_name) ||
            same_name != name) {
            return false;
        }

        const std::array facts{
            enum_source_fact{
                same_name,
                false,
                false,
                enum_definition_state::opaque,
                builtin_type::integer,
                {}
            }
        };

        const std::array batches{
            source_fact_batch{
                same_source,
                facts,
                {}
            }
        };

        if (!build_batches(
                builder,
                transaction,
                batches,
                diagnostics,
                9) ||
            !transaction.commit().ok()) {
            return false;
        }
    }

    return
        manager.compiled_graph().find(original) != nullptr &&
        manager.compiled_graph().find_id(name) == original;
}

bool test_aggregate_builtin_member() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;
    string_id type_name;
    string_id member_name;

    if (!resolve_source(transaction, source_a, source) ||
        !intern(transaction, "Record", type_name) ||
        !intern(transaction, "value", member_name)) {
        return false;
    }

    const std::array members{
        aggregate_source_fact::member_fact{
            member_name,
            builtin_type::integer,
            {},
            0,
            0
        }
    };

    const std::array aggregates{
        aggregate_source_fact{
            type_name,
            aggregate_definition_state::defined,
            members
        }
    };

    const std::array batches{
        source_fact_batch{
            source,
            {},
            aggregates
        }
    };

    if (!build_batches(
            builder,
            transaction,
            batches,
            diagnostics,
            10) ||
        !transaction.commit().ok()) {
        return false;
    }

    const auto identity =
        manager.compiled_graph().find_id(type_name);

    const auto* entity =
        manager.compiled_graph().find(identity);

    const auto* type =
        entity
            ? manager.compiled_graph().find(entity->type)
            : nullptr;

    const auto materialized =
        entity
            ? manager.compiled_graph().members(entity->type)
            : std::span<const member_record>{};

    if (!entity ||
        entity->kind != entity_kind::aggregate_type ||
        !type ||
        type->kind != user_type_kind::aggregate ||
        !type->definition ||
        materialized.size() != 1 ||
        materialized[0].name != member_name) {
        return false;
    }

    builtin_type builtin{};

    return
        manager.compiled_graph().builtin(
            materialized[0].type,
            builtin) &&
        builtin == builtin_type::integer;
}

bool test_aggregate_user_lvalue_reference() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;
    string_id target_name;
    string_id holder_name;
    string_id member_name;

    if (!resolve_source(transaction, source_a, source) ||
        !intern(transaction, "Target", target_name) ||
        !intern(transaction, "Holder", holder_name) ||
        !intern(transaction, "target", member_name)) {
        return false;
    }

    const std::array holder_modifiers{
        canonical_type_modifier{
            derived_type_kind::lvalue_reference,
            0
        }
    };

    const std::array holder_members{
        aggregate_source_fact::member_fact{
            member_name,
            std::nullopt,
            target_name,
            0,
            1
        }
    };

    const std::array aggregates{
        aggregate_source_fact{
            target_name,
            aggregate_definition_state::declared,
            {},
            {}
        },
        aggregate_source_fact{
            holder_name,
            aggregate_definition_state::defined,
            holder_members,
            holder_modifiers
        }
    };

    const std::array batches{
        source_fact_batch{
            source,
            {},
            aggregates
        }
    };

    if (!build_batches(
            builder,
            transaction,
            batches,
            diagnostics,
            11) ||
        !transaction.commit().ok()) {
        return false;
    }

    const auto target_id =
        manager.compiled_graph().find_id(target_name);

    const auto holder_id =
        manager.compiled_graph().find_id(holder_name);

    const auto* target =
        manager.compiled_graph().find(target_id);

    const auto* holder =
        manager.compiled_graph().find(holder_id);

    const auto members =
        holder
            ? manager.compiled_graph().members(holder->type)
            : std::span<const member_record>{};

    if (!target ||
        !holder ||
        members.size() != 1) {
        return false;
    }

    const auto* reference =
        manager.compiled_graph().derived(
            members[0].type);

    type_handle named{};

    return
        reference &&
        reference->kind ==
            derived_type_kind::lvalue_reference &&
        manager.compiled_graph().named(
            reference->child,
            named) &&
        named == target->type;
}

bool test_invalid_aggregate_member_is_fail_closed() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;
    string_id type_name;
    string_id member_name;

    if (!resolve_source(transaction, source_a, source) ||
        !intern(transaction, "InvalidRecord", type_name) ||
        !intern(transaction, "bad", member_name)) {
        return false;
    }

    const std::array members{
        aggregate_source_fact::member_fact{
            member_name,
            std::nullopt,
            {},
            0,
            0
        }
    };

    const std::array aggregates{
        aggregate_source_fact{
            type_name,
            aggregate_definition_state::defined,
            members
        }
    };

    const std::array batches{
        source_fact_batch{
            source,
            {},
            aggregates
        }
    };

    const auto result =
        builder.build(
            transaction,
            batches,
            operation_id{12},
            diagnostics);

    return
        !result.ok() &&
        result.code == status_code::configuration_failed &&
        !transaction.commit().ok() &&
        manager.state() == project_state::error &&
        manager.compiled_graph().entity_count() == 0;
}

bool build_identity_order(
    bool reverse,
    std::uint32_t& alpha,
    std::uint32_t& zeta) {

    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;
    string_id alpha_name;
    string_id zeta_name;

    if (!resolve_source(transaction, source_a, source)) {
        return false;
    }

    if (reverse) {
        if (!intern(transaction, "Zeta", zeta_name) ||
            !intern(transaction, "Alpha", alpha_name)) {
            return false;
        }
    }
    else {
        if (!intern(transaction, "Alpha", alpha_name) ||
            !intern(transaction, "Zeta", zeta_name)) {
            return false;
        }
    }

    const enum_source_fact alpha_fact{
        alpha_name,
        false,
        false,
        enum_definition_state::opaque,
        builtin_type::integer,
        {}
    };

    const enum_source_fact zeta_fact{
        zeta_name,
        false,
        false,
        enum_definition_state::opaque,
        builtin_type::integer,
        {}
    };

    std::array<enum_source_fact, 2> facts{};

    if (reverse) {
        facts = {zeta_fact, alpha_fact};
    }
    else {
        facts = {alpha_fact, zeta_fact};
    }

    const std::array batches{
        source_fact_batch{
            source,
            facts,
            {}
        }
    };

    if (!build_batches(
            builder,
            transaction,
            batches,
            diagnostics,
            13) ||
        !transaction.commit().ok()) {
        return false;
    }

    alpha =
        manager.compiled_graph().find_id(
            alpha_name).value();

    zeta =
        manager.compiled_graph().find_id(
            zeta_name).value();

    return
        alpha != 0 &&
        zeta != 0 &&
        alpha != zeta;
}

bool test_deterministic_stable_ids() {
    std::uint32_t alpha_forward = 0;
    std::uint32_t zeta_forward = 0;
    std::uint32_t alpha_reverse = 0;
    std::uint32_t zeta_reverse = 0;

    return
        build_identity_order(
            false,
            alpha_forward,
            zeta_forward) &&
        build_identity_order(
            true,
            alpha_reverse,
            zeta_reverse) &&
        alpha_forward == alpha_reverse &&
        zeta_forward == zeta_reverse &&
        alpha_forward < zeta_forward;
}

bool test_incremental_handle_and_typeref_preservation() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    source_id source;
    string_id target_name;
    string_id holder_name;
    string_id member_name;
    stable_id holder_id;
    type_handle holder_type;
    TypeRef member_type;

    const auto build_generation =
        [&](graph_build_mode mode,
            std::uint64_t operation) -> bool {

            auto transaction =
                manager.begin_build(mode);

            source_id current_source;
            string_id current_target;
            string_id current_holder;
            string_id current_member;

            if (!resolve_source(
                    transaction,
                    source_a,
                    current_source) ||
                !intern(
                    transaction,
                    "StableTarget",
                    current_target) ||
                !intern(
                    transaction,
                    "StableHolder",
                    current_holder) ||
                !intern(
                    transaction,
                    "target",
                    current_member)) {
                return false;
            }

            if (source &&
                (current_source != source ||
                 current_target != target_name ||
                 current_holder != holder_name ||
                 current_member != member_name)) {
                return false;
            }

            const std::array modifiers{
                canonical_type_modifier{
                    derived_type_kind::lvalue_reference,
                    0
                }
            };

            const std::array members{
                aggregate_source_fact::member_fact{
                    current_member,
                    std::nullopt,
                    current_target,
                    0,
                    1
                }
            };

            const std::array aggregates{
                aggregate_source_fact{
                    current_target,
                    aggregate_definition_state::declared,
                    {},
                    {}
                },
                aggregate_source_fact{
                    current_holder,
                    aggregate_definition_state::defined,
                    members,
                    modifiers
                }
            };

            const std::array batches{
                source_fact_batch{
                    current_source,
                    {},
                    aggregates
                }
            };

            diagnostics.clear();

            if (!build_batches(
                    builder,
                    transaction,
                    batches,
                    diagnostics,
                    operation) ||
                !transaction.commit().ok()) {
                return false;
            }

            source = current_source;
            target_name = current_target;
            holder_name = current_holder;
            member_name = current_member;
            return true;
        };

    if (!build_generation(
            graph_build_mode::rebuild,
            14)) {
        return false;
    }

    holder_id =
        manager.compiled_graph().find_id(
            holder_name);

    const auto* holder =
        manager.compiled_graph().find(holder_id);

    const auto first_members =
        holder
            ? manager.compiled_graph().members(
                  holder->type)
            : std::span<const member_record>{};

    if (!holder ||
        first_members.size() != 1) {
        return false;
    }

    holder_type = holder->type;
    member_type = first_members[0].type;

    if (!build_generation(
            graph_build_mode::incremental,
            15)) {
        return false;
    }

    const auto* after =
        manager.compiled_graph().find(holder_id);

    const auto after_members =
        after
            ? manager.compiled_graph().members(
                  after->type)
            : std::span<const member_record>{};

    return
        after &&
        after->type == holder_type &&
        after_members.size() == 1 &&
        after_members[0].type == member_type;
}

bool test_parser_publisher_boundary() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;

    if (!resolve_source(
            transaction,
            source_a,
            source)) {
        return false;
    }

    source_context context;
    source_name_ref enum_name;
    source_name_ref value_name;

    if (!context.store_name(
            "Published",
            enum_name).ok() ||
        !context.store_name(
            "One",
            value_name).ok()) {
        return false;
    }

    enum_value_source_fact value;
    value.name = value_name;
    value.value = {
        builtin_type::integer,
        1
    };

    context.enum_values.push_back(value);

    enum_declaration_source_fact declaration;
    declaration.canonical_name = enum_name;
    declaration.anonymous = false;
    declaration.scoped = false;
    declaration.definition_state =
        enum_definition_state::defined;
    declaration.explicit_underlying =
        builtin_type::integer;
    declaration.enumerator_offset = 0;
    declaration.enumerator_count = 1;

    context.enums.push_back(declaration);

    parser_source_fact_batch batch;
    batch.source = source;
    batch.context = &context;
    batch.enums = context.enums;
    batch.aggregates = context.aggregates;

    if (!publish_source_facts(
            transaction,
            batch,
            builder,
            operation_id{16},
            diagnostics).ok() ||
        !transaction.commit().ok()) {
        return false;
    }

    const auto canonical_name =
        manager.strings().find("Published");

    const auto canonical_value =
        manager.strings().find("One");

    const auto identity =
        manager.compiled_graph().find_id(
            canonical_name);

    const auto* entity =
        manager.compiled_graph().find(identity);

    const auto values =
        entity
            ? manager.compiled_graph().enum_values(
                  entity->type)
            : std::span<const enum_value_record>{};

    return
        canonical_name &&
        canonical_value &&
        identity &&
        entity &&
        values.size() == 1 &&
        values[0].name == canonical_value &&
        values[0].bits == 1;
}

bool test_parser_publisher_malformed_diagnostic() {
    graph_manager manager;
    project_builder builder;
    diagnostic_buffer diagnostics;

    if (!manager.initialize().ok()) {
        return false;
    }

    auto transaction =
        manager.begin_build(graph_build_mode::rebuild);

    source_id source;

    if (!resolve_source(
            transaction,
            source_a,
            source)) {
        return false;
    }

    parser_source_fact_batch batch;
    batch.source = source;
    batch.context = nullptr;

    const auto result =
        publish_source_facts(
            transaction,
            batch,
            builder,
            operation_id{17},
            diagnostics);

    if (result.ok() ||
        diagnostics.empty() ||
        transaction.commit().ok()) {
        return false;
    }

    bool found = false;

    for (const auto& record : diagnostics.records()) {
        if (record.id ==
            diagnostics::builder_invalid_source_fact.id) {
            found = true;
            break;
        }
    }

    return
        found &&
        manager.state() == project_state::error &&
        manager.compiled_graph().entity_count() == 0;
}

} // namespace

int main() {
    const struct test_case {
        const char* name;
        bool (*run)();
    } tests[] = {
        {"named enum materialization", test_named_enum_materialization},
        {"duplicate source diagnostic", test_duplicate_source_diagnostic_is_fail_closed},
        {"invalid Builder fact shape", test_invalid_builder_fact_shape},
        {"multi-source definition/removal", test_multi_source_definition_and_removal},
        {"identity resurrection", test_identity_resurrection_through_builder},
        {"aggregate builtin member", test_aggregate_builtin_member},
        {"aggregate user lvalue reference", test_aggregate_user_lvalue_reference},
        {"invalid aggregate fail-closed", test_invalid_aggregate_member_is_fail_closed},
        {"deterministic stable IDs", test_deterministic_stable_ids},
        {"incremental handle/TypeRef preservation", test_incremental_handle_and_typeref_preservation},
        {"Parser -> Publisher -> Builder boundary", test_parser_publisher_boundary},
        {"Publisher malformed diagnostic", test_parser_publisher_malformed_diagnostic}
    };

    for (const auto& test : tests) {
        if (!test.run()) {
            std::cerr << "FAILED: " << test.name << '\n';
            return 1;
        }

        std::cout << "PASS: " << test.name << '\n';
    }

    return 0;
}
