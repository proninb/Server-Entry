#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../project_root.hpp"

#include <filesystem>
#include <span>
#include <unordered_map>
#include <vector>

namespace cw::server
{
struct source_record { source_id id; std::filesystem::path path; };
struct source_root { source_id source; project_item_role role = project_item_role::source; };

class source_manager;

class source_manager_update final : public project_root_sink
{
public:
    source_manager_update(const source_manager_update&) = delete;
    source_manager_update& operator=(const source_manager_update&) = delete;
    source_manager_update(source_manager_update&& other) noexcept;
    source_manager_update& operator=(source_manager_update&&) = delete;

    [[nodiscard]] status add(const std::filesystem::path& path,
                             project_item_role role) noexcept override;
    [[nodiscard]] status commit() noexcept;

private:
    friend class source_manager;
    source_manager_update(source_manager& owner, std::uint32_t next_source_id,
                          std::uint64_t base_generation) noexcept
        : owner_(&owner), next_source_id_(next_source_id),
          base_generation_(base_generation) {}
    source_manager* owner_ = nullptr;
    std::vector<source_record> added_sources_;
    std::vector<source_root> roots_;
    std::unordered_map<std::filesystem::path, source_id> added_by_path_;
    std::uint32_t next_source_id_ = 1;
    std::uint64_t base_generation_ = 0;
    status failure_{};
    bool committed_ = false;
};

class source_manager
{
public:
    // SM1 is a single-writer transaction boundary. Concurrent calls require
    // external synchronization; generation validation is not a data-race lock.
    [[nodiscard]] status initialize() noexcept;
    [[nodiscard]] source_manager_update begin_update() noexcept;
    [[nodiscard]] const source_record* find(source_id id) const noexcept;
    [[nodiscard]] std::span<const source_root> roots() const noexcept;
    [[nodiscard]] std::span<const source_record> sources() const noexcept;

private:
    friend class source_manager_update;
    [[nodiscard]] status commit(source_manager_update& update) noexcept;
    std::vector<source_record> sources_;
    std::vector<source_root> roots_;
    std::unordered_map<std::filesystem::path, source_id> by_path_;
    std::uint32_t next_source_id_ = 1;
    std::uint64_t generation_ = 0;
};
} // namespace cw::server
