#include "../server_entry/project/implementation/implementation_parser.hpp"

#include "../server_entry/project/builder/project_builder.hpp"
#include "../server_entry/project/graph/graph_build_transaction.hpp"
#include "../server_entry/project/graph/graph_manager.hpp"
#include "../server_entry/project/source/source_manager.hpp"
#include "../server_entry/diagnostics/diagnostic_descriptor.hpp"
#include "../server_entry/metrics/source_acquisition_telemetry.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

using namespace cw::server;

std::filesystem::path make_source_file() {
    const auto nonce =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    const auto path =
        std::filesystem::temp_directory_path() /
        ("cw_implementation_frontend_" +
         std::to_string(nonce) +
         ".hpp");

    std::ofstream output{
        path,
        std::ios::binary | std::ios::trunc
    };

    output << '\n';
    output.close();

    return output.fail()
        ? std::filesystem::path{}
        : path;
}

bool build_test_graph(graph_manager& manager) {
    if (!manager.initialize().ok()) {
        return false;
    }

    const auto path =
        make_source_file();

    if (path.empty()) {
        return false;
    }

    auto transaction =
        manager.begin_build(
            graph_build_mode::rebuild);

    source_id source;

    if (!transaction.sources().resolve(
            path,
            project_item_role::type,
            source).ok()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return false;
    }

    source_acquisition_telemetry telemetry{
        metrics_mode::off
    };

    if (!transaction.sources().acquire(
            source,
            telemetry).ok()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return false;
    }

    string_id int_type_name;
    string_id box_type_name;
    string_id out_name;
    string_id in_name;
    string_id data_name;

    if (!transaction.strings().bind(
            "INT",
            int_type_name).ok() ||
        !transaction.strings().bind(
            "BOX",
            box_type_name).ok() ||
        !transaction.strings().bind(
            "OUT",
            out_name).ok() ||
        !transaction.strings().bind(
            "IN",
            in_name).ok() ||
        !transaction.strings().bind(
            "DATA",
            data_name).ok()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return false;
    }

    const std::array int_modifiers{
        canonical_type_modifier{
            derived_type_kind::lvalue_reference,
            0
        }
    };

    const std::array int_members{
        aggregate_source_fact::member_fact{
            out_name,
            builtin_type::integer,
            {},
            0,
            0,
            {}
        },
        aggregate_source_fact::member_fact{
            in_name,
            builtin_type::integer,
            {},
            0,
            1,
            {}
        }
    };

    const std::array box_modifiers{
        canonical_type_modifier{
            derived_type_kind::array,
            4
        }
    };

    const std::array box_members{
        aggregate_source_fact::member_fact{
            data_name,
            builtin_type::integer,
            {},
            0,
            1,
            {}
        }
    };

    const std::array aggregates{
        aggregate_source_fact{
            int_type_name,
            aggregate_definition_state::defined,
            int_members,
            int_modifiers,
            {}
        },
        aggregate_source_fact{
            box_type_name,
            aggregate_definition_state::defined,
            box_members,
            box_modifiers,
            {}
        }
    };

    const std::array batches{
        source_fact_batch{
            source,
            {},
            aggregates
        }
    };

    project_builder builder;
    diagnostic_buffer diagnostics;

    const auto built =
        builder.build(
            transaction,
            batches,
            operation_id{1},
            diagnostics);

    const auto committed =
        built.ok()
            ? transaction.commit()
            : built;

    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    return
        built.ok() &&
        committed.ok() &&
        manager.state() ==
            project_state::valid;
}

bool test_resolved_implementation_semantics() {
    graph_manager manager;

    if (!build_test_graph(manager)) {
        return false;
    }

    constexpr std::string_view source_text =
        "INT A, B;\n"
        "INT Grid[2][3];\n"
        "BOX Box;\n"
        "char C = 'X';\n"
        "char Message[8] = \"R\\n\\\"\";\n"
        "A.OUT = 3;\n"
        "B.IN = A.OUT;\n"
        "Grid[1][2].OUT = A.OUT;\n"
        "/* member array */ Box.DATA[2] = 9;\n"
        "// trailing comment\n";

    implementation_context context;

    const auto result =
        parse_implementation_source(
            {
                source_id{100},
                source_text
            },
            manager.compiled_graph(),
            manager.strings(),
            operation_id{2},
            context);

    if (!result.ok() ||
        !context.diagnostics.empty() ||
        context.objects.size() != 6 ||
        context.operations.size() != 6) {
        return false;
    }

    std::size_t literal_count = 0;
    std::size_t copy_count = 0;
    std::size_t binding_count = 0;

    for (const auto& operation :
         context.operations) {
        switch (operation.kind) {
        case implementation_operation_kind::value_literal:
            ++literal_count;
            break;
        case implementation_operation_kind::value_copy:
            ++copy_count;
            break;
        case implementation_operation_kind::binding:
            ++binding_count;
            break;
        }
    }

    if (literal_count != 4 ||
        copy_count != 1 ||
        binding_count != 1) {
        return false;
    }

    bool found_string = false;

    for (const auto& literal :
         context.literals) {
        if (literal.kind !=
            implementation_literal_kind::string) {
            continue;
        }

        if (literal.byte_count != 3 ||
            literal.byte_offset + literal.byte_count >
                context.literal_bytes.size()) {
            return false;
        }

        const auto* bytes =
            context.literal_bytes.data() +
            literal.byte_offset;

        if (bytes[0] != 'R' ||
            bytes[1] != '\n' ||
            bytes[2] != '"') {
            return false;
        }

        found_string = true;
    }

    if (!found_string) {
        return false;
    }

    bool found_member_array_path = false;
    bool found_multidimensional_path = false;

    for (const auto& path : context.paths) {
        if (path.step_count < 2) {
            continue;
        }

        const auto first =
            context.path_steps[
                path.step_offset];

        const auto second =
            context.path_steps[
                path.step_offset + 1];

        if (first.kind ==
                implementation_path_step_kind::member &&
            second.kind ==
                implementation_path_step_kind::index) {
            found_member_array_path = true;
        }

        if (first.kind ==
                implementation_path_step_kind::index &&
            second.kind ==
                implementation_path_step_kind::index) {
            found_multidimensional_path = true;
        }
    }

    if (!found_member_array_path ||
        !found_multidimensional_path) {
        return false;
    }

    implementation_facts_storage facts;

    if (!context.release_facts(
            source_id{100},
            facts).ok() ||
        facts.source != source_id{100} ||
        facts.objects.size() != 6 ||
        facts.operations.size() != 6) {
        return false;
    }

    std::string_view name;

    return
        facts.resolve_name(
            facts.objects[0].name,
            name).ok() &&
        name == "A";
}

bool has_diagnostic(
    const implementation_context& context,
    diagnostic_id id) {

    for (const auto& record :
         context.diagnostics.records()) {
        if (record.id == id) {
            return true;
        }
    }

    return false;
}

bool expect_failure(
    const graph_manager& manager,
    std::string_view source,
    diagnostic_id expected,
    std::uint64_t operation) {

    implementation_context context;

    const auto result =
        parse_implementation_source(
            {
                source_id{200},
                source
            },
            manager.compiled_graph(),
            manager.strings(),
            operation_id{operation},
            context);

    return
        !result.ok() &&
        result.code ==
            status_code::configuration_failed &&
        has_diagnostic(
            context,
            expected);
}

bool test_fail_closed_diagnostics() {
    graph_manager manager;

    if (!build_test_graph(manager)) {
        return false;
    }

    return
        expect_failure(
            manager,
            "Missing A;",
            diagnostics::implementation_unknown_type.id,
            10) &&
        expect_failure(
            manager,
            "INT A; A.Missing = 1;",
            diagnostics::implementation_unknown_member.id,
            11) &&
        expect_failure(
            manager,
            "INT A; Missing.OUT = 1;",
            diagnostics::implementation_unknown_object.id,
            12) &&
        expect_failure(
            manager,
            "INT A; INT A;",
            diagnostics::implementation_duplicate_object.id,
            13) &&
        expect_failure(
            manager,
            "INT A[2]; A[2].OUT = 1;",
            diagnostics::implementation_index_out_of_range.id,
            14) &&
        expect_failure(
            manager,
            "INT A, B; B.IN = 3;",
            diagnostics::implementation_type_mismatch.id,
            15) &&
        expect_failure(
            manager,
            "INT A, B; A = B;",
            diagnostics::implementation_type_mismatch.id,
            16) &&
        expect_failure(
            manager,
            "/* unterminated",
            diagnostics::implementation_invalid_source.id,
            17);
}

} // namespace

int main() {
    const struct test_case {
        const char* name;
        bool (*run)();
    } tests[] = {
        {
            "resolved implementation objects arrays literals and bindings",
            test_resolved_implementation_semantics
        },
        {
            "implementation diagnostics fail closed",
            test_fail_closed_diagnostics
        }
    };

    for (const auto& test : tests) {
        if (!test.run()) {
            std::cerr
                << "FAILED: "
                << test.name
                << '\n';
            return 1;
        }

        std::cout
            << "PASS: "
            << test.name
            << '\n';
    }

    return 0;
}
