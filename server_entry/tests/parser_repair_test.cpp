#include "project/parser/parser.hpp"
#include "project/parser/source_environment.hpp"
#include <cassert>
#include <iostream>
#include <string_view>

using namespace cw::server;

static std::string_view resolved(const source_context& c, source_name_ref r) {
    std::string_view out;
    assert(c.resolve_name(r, out).ok());
    return out;
}

int main() {
    {
        source_context ctx;
        source_environment env;
        const std::string_view text = "struct B; struct A { B* p[4]; B&& r; };";
        assert(parse_source({source_id{1}, text}, env, operation_id{}, ctx).ok());
        assert(ctx.aggregates.size() == 2);
        const auto members = ctx.members(ctx.aggregates[1]);
        assert(members.size() == 2);
        assert(resolved(ctx, members[0].type_name) == "B");
        auto m0 = ctx.modifiers(members[0]);
        assert(m0.size() == 2);
        assert(m0[0].kind == source_type_modifier_kind::pointer);
        assert(m0[1].kind == source_type_modifier_kind::array && m0[1].payload == 4);
        auto m1 = ctx.modifiers(members[1]);
        assert(m1.size() == 1 && m1[0].kind == source_type_modifier_kind::rvalue_reference);
    }

    source_type_binding binding{"", "B", "B"};
    source_environment_storage dep;
    assert(dep.initialize({}, std::span<const source_type_binding>{&binding, 1}).ok());

    {
        source_environment_import item{0, &dep};
        source_environment env{std::span<const source_environment_import>{&item, 1}};
        source_context ctx;
        const std::string_view text = "namespace N { struct A { B& b; }; }";
        assert(parse_source({source_id{2}, text}, env, operation_id{}, ctx).ok());
        const auto members = ctx.members(ctx.aggregates[0]);
        assert(resolved(ctx, members[0].type_name) == "B");
    }

    {
        source_environment_import item{1000, &dep};
        source_environment env{std::span<const source_environment_import>{&item, 1}};
        source_context ctx;
        const std::string_view text = "struct A { B b; };";
        assert(!parse_source({source_id{3}, text}, env, operation_id{}, ctx).ok());
    }

    // Redeclaration-aware exported interface.
    source_type_binding redecls[]{{"", "A", "A"}, {"", "A", "A"}};
    source_environment_storage iface;
    assert(iface.initialize({}, redecls).ok());

    // Transitive visibility.
    source_environment_storage mid;
    const source_environment_storage* imports[] {&dep};
    assert(mid.initialize({}, {}, imports).ok());
    source_environment_import item{0, &mid};
    source_environment transitive{std::span<const source_environment_import>{&item, 1}};
    std::string_view canonical;
    assert(transitive.find_type_exact("", "B", 10, canonical).ok());
    assert(canonical == "B");

    std::cout << "PASS\n";
}
