#pragma once

#include <cstdint>
#include <type_traits>

namespace cw::server {

class graph;
class graph_update;

// Identifies one entry in the canonical TypeRef table of a committed Graph
// generation. TypeRef is compact and generation-local; it is not stable Entity
// identity and must not be persisted or compared across unrelated Graph states
// except through the explicit compiled-state projection.
class TypeRef final {
public:
    constexpr TypeRef() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return index;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }

    friend constexpr bool operator==(
        TypeRef,
        TypeRef) noexcept = default;

private:
    explicit constexpr TypeRef(
        std::uint32_t value) noexcept
        : index(value) {}

    std::uint32_t index = 0;

    friend class graph;
    friend class graph_update;
};

static_assert(sizeof(TypeRef) == 4);
static_assert(std::is_trivially_copyable_v<TypeRef>);
static_assert(std::is_standard_layout_v<TypeRef>);

// Classifies the canonical representation stored at one TypeRef table entry.
enum class canonical_type_kind : std::uint8_t {
    builtin,
    named,
    derived
};

// Classifies one modifier wrapped around another canonical TypeRef.
// Derived types form a chain from the base type outward; each record references
// its immediate child, preserving modifier order in the canonical type graph.
enum class derived_type_kind : std::uint8_t {
    pointer,
    array,
    lvalue_reference,
    rvalue_reference
};

// Describes one derived canonical type.
// child is the immediately wrapped TypeRef. payload is zero for pointer/reference
// kinds and stores the array extent for array.
struct derived_type_record {
    derived_type_kind kind = derived_type_kind::pointer;
    TypeRef child{};
    std::uint64_t payload = 0;
};

static_assert(sizeof(derived_type_record) == 16);

} // namespace cw::server
