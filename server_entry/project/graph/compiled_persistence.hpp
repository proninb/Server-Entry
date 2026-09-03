#pragma once
#include "../../status.hpp"
#include <filesystem>
namespace cw::server { class graph; class string_registry; class metrics_store;
[[nodiscard]] status write_compiled_checkpoint(const std::filesystem::path&,const string_registry&,const graph&,metrics_store* = nullptr) noexcept;
[[nodiscard]] status read_compiled_checkpoint(const std::filesystem::path&,string_registry&,graph&,metrics_store* = nullptr) noexcept;
}
