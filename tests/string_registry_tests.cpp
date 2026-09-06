#include "../server_entry/project/string/string_registry.hpp"

#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace cw::server;

static_assert(
    !std::is_constructible_v<
        string_id,
        std::uint32_t>);

static_assert(
    !std::is_copy_constructible_v<
        string_registry>);

static_assert(
    !std::is_move_constructible_v<
        string_registry>);

bool test_canonical_binding_and_direct_slots() {
    string_registry registry;

    if (!registry.initialize().ok()) {
        return false;
    }

    auto update =
        registry.begin_update();

    if (!update.reserve_bindings(
            5,
            16).ok()) {
        return false;
    }

    string_id a;
    string_id repeated;
    string_id ab;
    string_id empty;
    string_id embedded;

    const std::string nul{
        "A\0B",
        3
    };

    if (!update.bind("A", a).ok() ||
        !a ||
        a.value() != 1 ||
        !update.bind(
            "A",
            repeated).ok() ||
        repeated != a ||
        !update.bind("AB", ab).ok() ||
        !ab ||
        ab == a ||
        !update.bind({}, empty).ok() ||
        !empty ||
        empty == a ||
        !update.bind(
            nul,
            embedded).ok() ||
        !embedded ||
        registry.size() != 0 ||
        !update.get(a).has_value() ||
        !update.commit().ok()) {
        return false;
    }

    const auto a_value =
        registry.get(a);

    const auto ab_value =
        registry.get(ab);

    const auto empty_value =
        registry.get(empty);

    const auto embedded_value =
        registry.get(embedded);

    return
        a_value &&
        *a_value == "A" &&
        ab_value &&
        *ab_value == "AB" &&
        empty_value &&
        empty_value->empty() &&
        embedded_value &&
        embedded_value->size() == 3 &&
        (*embedded_value)[1] == '\0' &&
        registry.size() == 4 &&
        registry.live_size() == 4;
}

bool test_growth_and_stale_generation() {
    string_registry registry;

    if (!registry.initialize().ok()) {
        return false;
    }

    string_id first;

    {
        auto update =
            registry.begin_update();

        if (!update.bind(
                "stable",
                first).ok() ||
            !update.commit().ok()) {
            return false;
        }
    }

    const auto before =
        registry.get(first);

    if (!before) {
        return false;
    }

    const auto* stable_bytes =
        before->data();

    constexpr std::uint32_t count =
        100'000;

    auto growth =
        registry.begin_update();

    if (!growth.reserve_bindings(
            count,
            count * 16).ok()) {
        return false;
    }

    for (std::uint32_t index = 0;
         index < count;
         ++index) {
        string_id id;

        const auto value =
            "value-" +
            std::to_string(index);

        if (!growth.bind(
                value,
                id).ok() ||
            id.value() !=
                index + 2) {
            return false;
        }
    }

    if (registry.size() != 1 ||
        growth.added_size() != count ||
        !growth.commit().ok()) {
        return false;
    }

    const auto after =
        registry.get(first);

    if (!after ||
        after->data() != stable_bytes ||
        *after != "stable" ||
        registry.size() !=
            count + 1) {
        return false;
    }

    auto first_update =
        registry.begin_update();

    auto stale =
        registry.begin_update();

    string_id committed_id;
    string_id stale_id;

    if (!first_update.bind(
            "committed",
            committed_id).ok() ||
        !stale.bind(
            "stale",
            stale_id).ok() ||
        committed_id != stale_id ||
        !first_update.commit().ok()) {
        return false;
    }

    string_id rejected;

    return
        stale.bind(
            "another",
            rejected).code ==
            status_code::invalid_state &&
        stale.commit().code ==
            status_code::invalid_state;
}

bool test_export_import_preserves_ids() {
    string_registry registry;

    if (!registry.initialize().ok()) {
        return false;
    }

    auto update =
        registry.begin_update();

    string_id a;
    string_id b;

    if (!update.bind("A", a).ok() ||
        !update.bind("B", b).ok() ||
        !update.commit().ok()) {
        return false;
    }

    std::vector<
        std::optional<std::string>>
        slots;

    if (!registry.export_slots(
            slots).ok()) {
        return false;
    }

    string_registry restored;

    if (!restored.initialize().ok() ||
        !restored.import_slots(
            slots).ok()) {
        return false;
    }

    const auto av =
        restored.get(a);

    const auto bv =
        restored.get(b);

    return
        av &&
        *av == "A" &&
        bv &&
        *bv == "B";
}

} // namespace

int main() {
    return
        test_canonical_binding_and_direct_slots() &&
        test_growth_and_stale_generation() &&
        test_export_import_preserves_ids()
        ? 0
        : 1;
}