#include "source_build_entry.hpp"

#include <limits>

namespace cw::server {

status source_build_entry::store_name(
    std::string_view value,
    build_name_ref& result) noexcept {

    result = {};

    if (value.empty() ||
        value.size() >
            (std::numeric_limits<std::uint32_t>::max)() ||
        names.size() >
            (std::numeric_limits<std::uint32_t>::max)() -
                value.size() ||
        name_entries.size() >=
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {
            status_code::configuration_failed
        };
    }

    try {
        source_build_name entry;

        entry.offset =
            static_cast<std::uint32_t>(
                names.size());

        entry.length =
            static_cast<std::uint32_t>(
                value.size());

        names.insert(
            names.end(),
            value.begin(),
            value.end());

        try {
            name_entries.push_back(
                entry);
        }
        catch (...) {
            names.resize(
                entry.offset);

            throw;
        }

        result.index =
            static_cast<std::uint32_t>(
                name_entries.size());

        return {};
    }
    catch (...) {
        result = {};
        return {
            status_code::initialization_failed
        };
    }
}

status source_build_entry::resolve_name(
    build_name_ref reference,
    std::string_view& output) const noexcept {

    output = {};

    if (!reference ||
        reference.index >
            name_entries.size()) {
        return {
            status_code::configuration_failed
        };
    }

    const auto& entry =
        name_entries[
            reference.index - 1];

    if (entry.offset > names.size() ||
        entry.length >
            names.size() -
                entry.offset) {
        return {
            status_code::configuration_failed
        };
    }

    output =
        std::string_view{
            names.data() +
                entry.offset,
            entry.length
        };

    return {};
}

status source_build_entry::bind_name(
    build_name_ref reference,
    string_id canonical) noexcept {

    if (!reference ||
        !canonical ||
        reference.index >
            name_entries.size()) {
        return {
            status_code::configuration_failed
        };
    }

    auto& entry =
        name_entries[
            reference.index - 1];

    if (entry.canonical &&
        entry.canonical != canonical) {
        return {
            status_code::invalid_state
        };
    }

    entry.canonical =
        canonical;

    return {};
}

string_id source_build_entry::bound_name(
    build_name_ref reference) const noexcept {

    return
        reference &&
        reference.index <=
            name_entries.size()
        ? name_entries[
              reference.index - 1].canonical
        : string_id{};
}

void source_build_entry::reset() noexcept {
    source = {};
    names.clear();
    name_entries.clear();
    enum_values.clear();
    enums.clear();
    modifiers.clear();
    members.clear();
    aggregates.clear();
}

} // namespace cw::server