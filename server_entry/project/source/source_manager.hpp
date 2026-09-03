#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../project_root.hpp"
#include "source_hash.hpp"
#include "source_view.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cw::server
{
class metrics_store;
struct source_record { source_id id; std::filesystem::path path; };
struct source_root { source_id source; project_item_role role = project_item_role::source; };
static_assert(sizeof(source_id) == 4);

enum class source_presence : std::uint8_t { present, missing };
struct source_observation
{
    // Canonical Windows FILETIME: 100 ns ticks since 1601-01-01 UTC.
    std::uint64_t write_time_ticks = 0;
    std::uintmax_t size = 0;
    friend bool operator==(const source_observation&, const source_observation&) noexcept = default;
};
struct source_physical_state
{
    source_presence presence = source_presence::missing;
    source_observation observation{};
    source_content_hash hash{};
};
enum class source_change_kind : std::uint8_t { added, modified, removed };
struct source_change { source_id source; source_change_kind kind; };

struct source_physical_storage;
struct source_physical_delta;
class metrics_store;
class source_acquisition_telemetry;
class diagnostic_buffer;
class operation_id;

class source_manager;
class stable_source_manager_view;
class graph_build_transaction;
class graph_build_transaction_test_access;
class graph_update;

class source_manager_update final : public project_root_sink
{
public:
    ~source_manager_update();
    source_manager_update(const source_manager_update&) = delete;
    source_manager_update& operator=(const source_manager_update&) = delete;
    source_manager_update(source_manager_update&& other) noexcept;
    source_manager_update& operator=(source_manager_update&&) = delete;

    [[nodiscard]] status add(const std::filesystem::path& path,
                             project_item_role role) noexcept override;
    [[nodiscard]] status resolve(const std::filesystem::path& normalized_path,
                                 project_item_role role, source_id& output) noexcept;
    [[nodiscard]] status resolve_include(const std::filesystem::path& normalized_path,
                                         source_id& output) noexcept;
    [[nodiscard]] status resolve_include(source_id including_source,
                                         std::string_view relative_path,
                                         source_id& output) noexcept;
    [[nodiscard]] status set_includes(source_id source,
                                      std::span<const source_id> includes) noexcept;
    [[nodiscard]] status validate_source_graph(operation_id operation,
                                               diagnostic_buffer& diagnostics) noexcept;
    [[nodiscard]] std::span<const source_id> includes(source_id source) const noexcept;
    [[nodiscard]] status acquire(source_id id,
                                 source_acquisition_telemetry& telemetry) noexcept;
    // Candidate views are stable while this update is active, except that
    // reacquiring the same source may replace its candidate state. All views
    // obtained from an update are contractually invalid after commit/discard.
    [[nodiscard]] status get_view(source_id id, source_view& output) const noexcept;
    [[nodiscard]] status get_physical_state(source_id id,
                                            source_physical_state& output) const noexcept;
    [[nodiscard]] std::span<const source_change> changes() const noexcept;
    [[nodiscard]] std::span<const source_root> roots() const noexcept;
    // Records the current net candidate classification when orchestration
    // chooses its publication/measurement boundary. This is not a BUILD
    // publication signal and is intentionally separate from commit().
    void record_candidate_metrics(metrics_store& metrics) const noexcept;
    [[nodiscard]] status commit() noexcept;

private:
    friend class source_manager;
    friend class graph_build_transaction;
    friend class graph_update;
    source_manager_update(source_manager& owner, std::uint32_t next_source_id,
                          std::uint64_t base_generation) noexcept;
    source_manager* owner_ = nullptr;
    std::vector<source_record> added_sources_;
    std::vector<source_root> roots_;
    std::unordered_map<std::filesystem::path, source_id> added_by_path_;
    std::unordered_map<std::uint32_t, std::unique_ptr<source_physical_delta>> physical_delta_;
    std::unordered_map<std::uint32_t, std::vector<source_id>> include_delta_;
    std::unordered_map<std::uint32_t, std::vector<source_id>> prepared_dependents_;
    std::vector<source_change> changes_;
    std::uint32_t next_source_id_ = 1;
    std::uint64_t base_generation_ = 0;
    status failure_{};
    bool committed_ = false;
    bool prepared_ = false;
    bool graph_validated_ = true;

    [[nodiscard]] status prepare_publish() noexcept;
    void publish_prepared() noexcept;
    void cancel() noexcept;
    [[nodiscard]] bool contains_for_validation(source_id id) const noexcept;
    [[nodiscard]] std::span<const source_id> candidate_includes(source_id id) const noexcept;
};

class source_manager
{
public:
    source_manager();
    ~source_manager();
    // SM1 is a single-writer transaction boundary. Concurrent calls require
    // external synchronization; generation validation is not a data-race lock.
    [[nodiscard]] status initialize() noexcept;
    [[nodiscard]] source_manager_update begin_update() noexcept;
    [[nodiscard]] const source_record* find(source_id id) const noexcept;
    // A committed view remains valid until this source's committed physical
    // state is replaced or the Source Manager is destroyed.
    [[nodiscard]] status get_view(source_id id, source_view& output) const noexcept;
    [[nodiscard]] status get_physical_state(source_id id,
                                            source_physical_state& output) const noexcept;
    [[nodiscard]] std::span<const source_root> roots() const noexcept;
    [[nodiscard]] std::span<const source_record> sources() const noexcept;
    [[nodiscard]] std::span<const source_id> includes(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_id> dependents(source_id source) const noexcept;
    [[nodiscard]] status collect_dependents(source_id source,
                                            std::vector<source_id>& output) const noexcept;
    [[nodiscard]] std::size_t source_count() const noexcept;
    [[nodiscard]] std::size_t root_count() const noexcept;
    [[nodiscard]] status get_path(source_id id,
                                  std::filesystem::path& output) const noexcept;
    [[nodiscard]] status find_by_path(const std::filesystem::path& normalized_path,
                                      source_id& output) const noexcept;
    [[nodiscard]] status get_root(std::size_t index, source_root& output) const noexcept;
    [[nodiscard]] status get_dependencies(source_id source, bool reverse,
                                           std::vector<source_id>& output) const noexcept;
    [[nodiscard]] status save_checkpoint(const std::filesystem::path& path) const noexcept;
    [[nodiscard]] status save_checkpoint(const std::filesystem::path& path,
                                         metrics_store* metrics) const noexcept;
    [[nodiscard]] status load_checkpoint(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static status strict_validate_checkpoint(
        const std::filesystem::path& path) noexcept;

private:
    friend class source_manager_update;
    friend class graph_build_transaction;
    friend class graph_build_transaction_test_access;
    std::vector<source_record> sources_;
    std::vector<source_root> roots_;
    std::unordered_map<std::filesystem::path, source_id> by_path_;
    std::vector<std::unique_ptr<source_physical_storage>> physical_;
    std::vector<std::vector<source_id>> includes_;
    std::vector<std::vector<source_id>> dependents_;
    std::uint32_t next_source_id_ = 1;
    std::uint64_t generation_ = 0;
    std::unique_ptr<stable_source_manager_view> stable_;
};
} // namespace cw::server
