#pragma once

namespace cw::server
{
struct preprocessor_capabilities
{
    bool include = true;
    bool define = false;
    bool conditionals = false;
    bool macros = false;
};

struct language_configuration
{
    preprocessor_capabilities preprocessor;
    bool include_inside_namespace = false;
};
} // namespace cw::server
