#pragma once

#include "../source_id.hpp"

#include <cstdint>

namespace cw::server {

// Dense one-based coordinate of one named declaration inside a Source parse.
class source_declaration_id final {
public:
    constexpr source_declaration_id() noexcept = default;

    explicit constexpr source_declaration_id(std::uint32_t value) noexcept
        : slot(value) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return slot;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return slot != 0;
    }

    friend constexpr bool operator==(
        source_declaration_id,
        source_declaration_id) noexcept = default;

private:
    std::uint32_t slot = 0;
};

// Source-language semantic identity selected by Parser name resolution.
// This is not type_id and is never Runtime-visible.
struct source_entity_ref {
    source_id source{};
    source_declaration_id declaration{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(source) &&
            static_cast<bool>(declaration);
    }

    friend constexpr bool operator==(
        source_entity_ref,
        source_entity_ref) noexcept = default;
};

} // namespace cw::server