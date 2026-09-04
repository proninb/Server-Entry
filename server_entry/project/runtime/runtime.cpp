#include "runtime.hpp"

#include "../graph/graph.hpp"

namespace cw::server {

status runtime::attach(const graph& graph) noexcept {
    static_cast<void>(graph);
    return {};
}

} // namespace cw::server
