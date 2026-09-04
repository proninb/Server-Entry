#include "source_build_entry.hpp"

#include <limits>

namespace cw::server {

status source_build_entry::store_name(
    std::string_view value,
    build_name_ref& result) noexcept {

    result = {};

    if (value.empty() ||
        value.size() > std::numeric_limits<std::uint32_t>::max() ||
        names.size() > std::numeric_limits<std::uint32_t>::max() - value.size()) {
        return {status_code::configuration_failed};
    }

    try {
        result.offset = static_cast<std::uint32_t>(names.size());
        result.length = static_cast<std::uint32_t>(value.size());
        names.insert(names.end(), value.begin(), value.end());
        return {};
    }
    catch (...) {
        result = {};
        return {status_code::initialization_failed};
    }
}

status source_build_entry::resolve_name(
    build_name_ref reference,
    std::string_view& output) const noexcept {

    output = {};

    if (!reference ||
        reference.offset > names.size() ||
        reference.length > names.size() - reference.offset) {
        return {status_code::configuration_failed};
    }

    output = std::string_view{
        names.data() + reference.offset,
        reference.length
    };

    return {};
}

void source_build_entry::reset() noexcept {
    source = {};
    names.clear();
    enum_values.clear();
    enums.clear();
    modifiers.clear();
    members.clear();
    aggregates.clear();
}

} // namespace cw::server
