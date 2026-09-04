#pragma once

#include "../../status.hpp"

namespace cw::server {

    // Owns the Server-side Shared Memory publication boundary for one Project.
    // shared_memory is responsible for creating and managing the memory region used
    // to expose runtime state; Project and Graph construction remain outside this layer.
    class shared_memory {
    public:
        [[nodiscard]] status initialize() noexcept;
    };

} // namespace cw::server
