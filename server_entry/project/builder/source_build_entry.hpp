#pragma once

#include "../parser/source_facts.hpp"

namespace cw::server {

// Compatibility name for Builder/frontend code. The concrete owner is the
// Parser-layer Source semantic product; there is no second captured copy.
using source_build_entry = source_facts_storage;

} // namespace cw::server
