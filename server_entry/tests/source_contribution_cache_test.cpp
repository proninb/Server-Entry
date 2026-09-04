#include "project/builder/source_contribution_cache.hpp"

#include <cassert>
#include <iostream>

using namespace cw::server;

int main() {
    source_contribution_cache cache;
    assert(cache.initialize().ok());

    {
        auto update = cache.begin_update(false);
        source_contribution_state* state = nullptr;
        assert(update.replace(source_id{1}, state).ok());
        assert(state != nullptr);

        source_contribution_record contribution;
        state->named.push_back(contribution);

        assert(update.prepare_publish().ok());
        update.publish_prepared();
    }

    assert(cache.contribution_count(source_id{1}) == 1);

    {
        // G0 rebuild sees only Source 2. Source 1 must not survive in the
        // non-authoritative contribution cache after publication.
        auto update = cache.begin_update(true);
        source_contribution_state* state = nullptr;
        assert(update.replace(source_id{2}, state).ok());
        assert(state != nullptr);

        source_contribution_record contribution;
        state->named.push_back(contribution);

        assert(update.prepare_publish().ok());
        update.publish_prepared();
    }

    assert(cache.contribution_count(source_id{1}) == 0);
    assert(cache.contribution_count(source_id{2}) == 1);
    assert(cache.complete());

    cache.invalidate();
    assert(!cache.complete());
    assert(cache.contribution_count(source_id{2}) == 0);

    std::cout << "PASS\n";
}
