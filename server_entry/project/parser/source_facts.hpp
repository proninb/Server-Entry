#pragma once

#include "../source_entity_ref.hpp"
#include "../../member_index.hpp"
#include "../graph/builtin_type.hpp"
#include "../language/aggregate_semantics.hpp"
#include "../language/enum_semantics.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

// Identifies a half-open byte range inside the immutable Source snapshot that
// produced these facts. Ranges are diagnostics/provenance, not Entity identity.
struct source_text_range {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

// References one spelling in the owning Source semantic storage.
// index is a dense one-based publication-binding coordinate; semantic relations
// still use source_entity_ref after Parser resolution.
struct source_name_ref {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t index = 0;

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return length != 0 && index != 0;
    }
};

// Dense Parser-context record for one named type declaration.
struct source_type_declaration {
    source_entity_ref entity{};
    source_name_ref canonical_name{};
    source_text_range name_range{};
};

struct enum_value_source_fact {
    source_name_ref name{};
    integral_constant value{};
    source_text_range name_range{};
    source_text_range expression_range{};
};

struct enum_declaration_source_fact {
    source_entity_ref entity{};
    source_name_ref canonical_name{};
    source_name_ref scope_name{};

    bool anonymous = false;
    bool scoped = false;

    enum_definition_state definition_state =
        enum_definition_state::defined;

    std::optional<builtin_type> explicit_underlying;

    std::uint32_t enumerator_offset = 0;
    std::uint32_t enumerator_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
    source_text_range underlying_range{};
};

struct aggregate_declaration_source_fact {
    source_entity_ref entity{};
    source_name_ref canonical_name{};
    source_name_ref scope_name{};

    aggregate_definition_state definition_state =
        aggregate_definition_state::declared;

    std::uint32_t member_offset = 0;
    std::uint32_t member_count = 0;

    std::uint32_t construction_binding_offset = 0;
    std::uint32_t construction_binding_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
};

enum class source_type_modifier_kind : std::uint8_t {
    pointer,
    array,
    lvalue_reference,
    rvalue_reference
};

struct source_type_modifier {
    source_type_modifier_kind kind =
        source_type_modifier_kind::pointer;

    std::uint64_t payload = 0;
    source_text_range range{};
};

// type_entity is authoritative after successful user-type resolution.
// type_name is retained only for diagnostics/source-interface provenance.
struct member_declaration_source_fact {
    source_name_ref name{};
    source_name_ref type_name{};

    std::optional<builtin_type> builtin;

    std::uint32_t modifier_offset = 0;
    std::uint32_t modifier_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
    source_text_range type_range{};

    source_entity_ref type_entity{};
};

// Source-local internal-linkage storage declared by one Type Source.
// canonical_name is Parser identity only; publication never admits it to the
// Project-global String Registry or stable Entity namespace.
struct static_object_source_fact {
    source_name_ref canonical_name{};
    source_name_ref type_name{};

    std::optional<builtin_type> builtin;

    std::uint32_t modifier_offset = 0;
    std::uint32_t modifier_count = 0;

    source_text_range declaration_range{};
    source_text_range name_range{};
    source_text_range type_range{};

    source_entity_ref type_entity{};
};

// Resolved declarative default-construction binding.
// static_object is a one-based Source-local coordinate into static_objects.
struct construction_binding_source_fact {
    member_index member{};
    std::uint32_t static_object = 0;

    source_text_range member_range{};
    source_text_range object_range{};
};

class source_context;

// Owns the immutable Source-language semantic product after Parser completion.
// It contains no project-canonical identity, ABI layout, Graph or Runtime state.
// Publication performs the only Source-spelling canonicalization boundary.
class source_facts_storage final {
public:
    [[nodiscard]] status resolve_name(
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

    // Returns one Source-local spelling together with the hash prepared by the
    // Parser worker. The hash is only a String Registry acceleration hint;
    // canonical equality remains byte equality.
    [[nodiscard]] status resolve_name_binding(
        source_name_ref reference,
        std::string_view& output,
        std::uint64_t& hash) const noexcept {

        hash = 0;

        const auto result =
            resolve_name(
                reference,
                output);

        if (!result.ok()) {
            return result;
        }

        if (reference.index >
            name_hashes.size()) {
            output = {};
            return {
                status_code::configuration_failed
            };
        }

        hash =
            name_hashes[
                reference.index - 1];

        return {};
    }

    [[nodiscard]] std::size_t name_count() const noexcept {
        return stored_name_count;
    }

    [[nodiscard]] std::size_t name_bytes_size() const noexcept {
        return names.size();
    }

    void reset() noexcept {
        source = {};
        stored_name_count = 0;
        names.clear();
        name_hashes.clear();
        enum_values.clear();
        enums.clear();
        modifiers.clear();
        members.clear();
        aggregates.clear();
        static_objects.clear();
        construction_bindings.clear();
    }

    source_id source{};
    std::vector<enum_value_source_fact> enum_values;
    std::vector<enum_declaration_source_fact> enums;
    std::vector<source_type_modifier> modifiers;
    std::vector<member_declaration_source_fact> members;
    std::vector<aggregate_declaration_source_fact> aggregates;
    std::vector<static_object_source_fact> static_objects;
    std::vector<construction_binding_source_fact> construction_bindings;

private:
    friend class source_context;

    std::uint32_t stored_name_count = 0;
    std::vector<char> names;
    std::vector<std::uint64_t> name_hashes;
};

} // namespace cw::server
