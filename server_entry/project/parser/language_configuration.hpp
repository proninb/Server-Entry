#pragma once

namespace cw::server {

// Declares which preprocessing features are supported by one source language.
// These capabilities let Source Manager and Parser orchestration reason about
// include handling without embedding language-specific preprocessing rules.
struct preprocessor_capabilities {
    bool include = true;
    bool define = false;
    bool conditionals = false;
    bool macros = false;
};

// Defines the source-language capabilities required by parsing and source
// dependency resolution for one language frontend. Includes inside namespaces\n// are intentionally unsupported by the current separate-Source model.
struct language_configuration {
    preprocessor_capabilities preprocessor;
};

} // namespace cw::server
