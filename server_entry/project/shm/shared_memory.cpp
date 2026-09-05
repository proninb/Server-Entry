#include "shared_memory.hpp"

namespace cw::server {

status shared_memory::initialize() noexcept {
    // Current SHM layer is only an architectural boundary. Mapping/configuration
    // is not represented by shared_memory yet, so initialization is a no-op.
    return {};
}

}