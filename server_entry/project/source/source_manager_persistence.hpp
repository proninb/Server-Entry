#pragma once

#include "source_manager.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cw::server
{
class metrics_store;
[[nodiscard]] std::uint64_t source_path_xxh64(std::string_view bytes) noexcept;
[[nodiscard]] std::uint32_t source_path_fingerprint(std::uint64_t hash) noexcept;
[[nodiscard]] std::uint32_t source_manager_crc32c(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] status encode_wtf8(std::wstring_view input, std::string& output) noexcept;
[[nodiscard]] status decode_wtf8(std::string_view input, std::wstring& output) noexcept;

class stable_source_manager_view final
{
public:
    stable_source_manager_view() noexcept;
    ~stable_source_manager_view();
    stable_source_manager_view(const stable_source_manager_view&) = delete;
    stable_source_manager_view& operator=(const stable_source_manager_view&) = delete;

    [[nodiscard]] status open(const std::filesystem::path& path) noexcept;
    [[nodiscard]] status validate_artifact() const noexcept;
    [[nodiscard]] status strict_validate() const noexcept;
    [[nodiscard]] std::size_t source_count() const noexcept;
    [[nodiscard]] std::size_t root_count() const noexcept;
    [[nodiscard]] status path(source_id id, std::filesystem::path& output) const noexcept;
    [[nodiscard]] status find(std::filesystem::path const& path, source_id& output) const noexcept;
    [[nodiscard]] status physical(source_id id, source_physical_state& output) const noexcept;
    [[nodiscard]] status root(std::size_t index, source_root& output) const noexcept;
    [[nodiscard]] status dependencies(source_id id, bool reverse,
                                      std::vector<source_id>& output) const noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

[[nodiscard]] status write_source_manager_checkpoint(
    const source_manager& manager, const std::filesystem::path& path,
    metrics_store* metrics = nullptr) noexcept;
[[nodiscard]] status strict_validate_source_manager_checkpoint(
    const std::filesystem::path& path) noexcept;
}
