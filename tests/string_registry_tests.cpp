#include "../server_entry/project/string/string_registry.hpp"

#include <string>
#include <string_view>
#include <type_traits>

namespace
{
using namespace cw::server;

static_assert(!std::is_constructible_v<string_id, std::uint32_t>);
static_assert(!std::is_copy_constructible_v<string_registry>);
static_assert(!std::is_move_constructible_v<string_registry>);

bool test_identity_and_values()
{
    string_registry registry;
    if (!registry.initialize().ok()) return false;
    const string_id invalid;
    if (invalid || invalid.value() != 0 || registry.get(invalid).has_value()) return false;

    string_id a, repeated, ab, empty, embedded;
    const std::string nul{"A\0B", 3};
    auto update = registry.begin_update();
    if (!update.intern("A", a).ok() || !a || a.value() != 1 ||
        !update.intern("A", repeated).ok() || repeated != a ||
        !update.intern("AB", ab).ok() || ab == a ||
        !update.intern({}, empty).ok() || !empty || empty == a ||
        !update.intern(nul, embedded).ok() || !embedded || registry.size() != 0 ||
        registry.find("A") || !update.get(empty).has_value() || !update.commit().ok())
        return false;

    const auto a_value = registry.get(a);
    const auto ab_value = registry.get(ab);
    const auto empty_value = registry.get(empty);
    const auto embedded_value = registry.get(embedded);
    return a_value && *a_value == "A" && ab_value && *ab_value == "AB" &&
           empty_value && empty_value->empty() && embedded_value &&
           embedded_value->size() == 3 && (*embedded_value)[1] == '\0' &&
           registry.find("A") == a && registry.find("missing") == string_id{};
}

bool test_growth_rebuild_and_epoch_reset()
{
    string_registry registry;
    if (!registry.initialize().ok()) return false;
    string_id first;
    { auto update = registry.begin_update();
      if (!update.intern("stable", first).ok() || !update.commit().ok()) return false; }
    const auto before = registry.get(first);
    if (!before) return false;
    const auto* stable_bytes = before->data();

    constexpr std::uint32_t count = 100'000;
    auto growth = registry.begin_update();
    for (std::uint32_t index = 0; index < count; ++index)
    {
        string_id id;
        const auto value = "value-" + std::to_string(index);
        if (!growth.intern(value, id).ok() || id.value() != index + 2) return false;
    }
    if (registry.size() != 1 || growth.added_size() != count || !growth.commit().ok())
        return false;
    const auto after = registry.get(first);
    if (!after || after->data() != stable_bytes || *after != "stable" ||
        registry.size() != count + 1) return false;

    if (!registry.rebuild_lookup_index().ok() || registry.find("stable") != first)
        return false;
    for (std::uint32_t index : {0u, 1u, 99u, 9'999u, 99'999u})
    {
        const auto value = "value-" + std::to_string(index);
        const auto id = registry.find(value);
        const auto stored = registry.get(id);
        if (!id || id.value() != index + 2 || !stored || *stored != value) return false;
    }

    string_id next;
    { auto update = registry.begin_update();
      if (!update.intern("next", next).ok() || next.value() != count + 2 ||
          !update.commit().ok()) return false; }

    const auto size_before_discard = registry.size();
    { auto discarded = registry.begin_update(); string_id id;
      if (!discarded.intern("discarded", id).ok()) return false; }
    if (registry.size() != size_before_discard || registry.find("discarded")) return false;

    auto first_update = registry.begin_update();
    auto stale = registry.begin_update();
    string_id committed_id, stale_id;
    if (!first_update.intern("committed", committed_id).ok() ||
        !stale.intern("stale", stale_id).ok() || committed_id != stale_id ||
        !first_update.commit().ok() || stale.find("stale") || stale.get(stale_id).has_value())
        return false;
    string_id rejected;
    if (stale.intern("another", rejected).code != status_code::invalid_state ||
        stale.commit().code != status_code::invalid_state ||
        registry.find("stale")) return false;

    if (!registry.initialize().ok() || registry.size() != 0 || registry.find("stable"))
        return false;
    string_id fresh;
    auto fresh_update = registry.begin_update();
    return fresh_update.intern("fresh", fresh).ok() && fresh.value() == 1 &&
           fresh_update.commit().ok();
}
} // namespace

int main()
{
    return test_identity_and_values() && test_growth_rebuild_and_epoch_reset() ? 0 : 1;
}
