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

namespace cw::server {

class metrics_store;
class source_acquisition_telemetry;
class diagnostic_buffer;
class operation_id;
class stable_source_manager_view;
class graph_build_transaction;
class graph_build_transaction_test_access;
class graph_update;

struct source_physical_storage;
struct source_physical_delta;

// Represents one Source known to Source Manager.
// source_id is the dense Source identity; path is the normalized filesystem
// identity used for lookup and acquisition.
struct source_record {
    source_id id;
    std::filesystem::path path;
};

// Identifies one root Source contributed by Project composition.
// Roots seed Source traversal but do not encode dependency relationships.
struct source_root {
    source_id source;
    project_item_role role = project_item_role::source;
};

static_assert(sizeof(source_id) == 4);

// Describes whether the current physical Source exists on the filesystem.
enum class source_presence : std::uint8_t {
    present,
    missing
};

// Captures filesystem metadata used by Source acquisition change detection.
// write_time_ticks uses one canonical representation on all platforms:
// 100 ns ticks since 1601-01-01 UTC, matching Windows FILETIME units.
struct source_observation {
    std::uint64_t write_time_ticks = 0;
    std::uintmax_t size = 0;

    friend bool operator==(
        const source_observation&,
        const source_observation&) noexcept = default;
};

// Represents the committed or candidate physical state of one Source.
// Content bytes are stored separately and are addressed through source_view.
struct source_physical_state {
    source_presence presence = source_presence::missing;
    source_observation observation{};
    source_content_hash hash{};
};

// Classifies the net physical Source change relative to committed state.
enum class source_change_kind : std::uint8_t {
    added,
    modified,
    removed
};

struct source_change {
    source_id source;
    source_change_kind kind;
};

// Immutable Source-local input captured by Source Manager before filesystem work.
// Workers may execute this job without accessing or mutating Source Manager.
struct source_acquire_job {
    source_id source{};
    std::filesystem::path path;
    source_physical_state committed{};
    source_physical_state baseline{};
    bool has_committed = false;
    bool has_baseline = false;
};

enum class source_acquire_result_kind : std::uint8_t {
    unchanged,
    missing,
    present
};

// Worker-produced filesystem/hash result. It owns acquired bytes until the
// coordinator applies them to the candidate Source state.
struct source_acquire_result {
    source_id source{};
    source_acquire_result_kind kind = source_acquire_result_kind::unchanged;
    source_observation observation{};
    source_content_hash hash{};
    std::string content;
};

class source_manager;

// Builds one isolated candidate update against a Source Manager generation.
// The update owns newly resolved Sources, physical acquisition deltas, root
// selection, and dependency replacements until two-phase publication succeeds.
// One update is owned by the build coordinator. Filesystem acquisition may run
// concurrently only through immutable source_acquire_job values; workers never
// access or mutate this update directly.
class source_manager_update final : public project_root_sink {
public:
    ~source_manager_update();

    source_manager_update(const source_manager_update&) = delete;
    source_manager_update& operator=(const source_manager_update&) = delete;

    source_manager_update(source_manager_update&& other) noexcept;
    source_manager_update& operator=(source_manager_update&&) = delete;

    [[nodiscard]] status add(
        const std::filesystem::path& path,
        project_item_role role) noexcept override;

    [[nodiscard]] status resolve(
        const std::filesystem::path& normalized_path,
        project_item_role role,
        source_id& output) noexcept;

    [[nodiscard]] status resolve_include(
        const std::filesystem::path& normalized_path,
        source_id& output) noexcept;

    [[nodiscard]] status resolve_include(
        source_id including_source,
        std::string_view relative_path,
        source_id& output) noexcept;

    [[nodiscard]] status set_includes(
        source_id source,
        std::span<const source_id> includes) noexcept;

    [[nodiscard]] status validate_source_graph(
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    [[nodiscard]] std::span<const source_id>
        includes(source_id source) const noexcept;

    // Captures Source-local baseline state for a filesystem acquisition job.
    // The returned job is immutable and can be executed on any worker thread.
    [[nodiscard]] status prepare_acquire(
        source_id id,
        source_acquire_job& job) noexcept;

    // Performs only filesystem snapshot/read/hash work. It does not access the
    // Source Manager or any transaction-owned mutable state.
    [[nodiscard]] static status execute_acquire(
        const source_acquire_job& job,
        source_acquisition_telemetry& telemetry,
        source_acquire_result& result) noexcept;

    // Applies one completed Source-local acquisition result to the candidate.
    // In the target architecture this is called by the build coordinator.
    [[nodiscard]] status apply_acquire(
        const source_acquire_job& job,
        source_acquire_result&& result,
        source_acquisition_telemetry& telemetry) noexcept;

    // Compatibility convenience path: prepare -> execute -> apply. New parallel
    // orchestration should schedule execute_acquire() outside Source Manager.
    [[nodiscard]] status acquire(
        source_id id,
        source_acquisition_telemetry& telemetry) noexcept;

    // Candidate views are stable while this update is active, except that
    // reacquiring the same Source may replace its candidate state. Every view
    // obtained from an update becomes invalid after commit or cancellation.
    [[nodiscard]] status get_view(
        source_id id,
        source_view& output) const noexcept;

    [[nodiscard]] status get_physical_state(
        source_id id,
        source_physical_state& output) const noexcept;

    // Returns the current net change set. Record order is an implementation
    // detail and must not be used as Source semantic or publication order.
    [[nodiscard]] std::span<const source_change> changes() const noexcept;
    [[nodiscard]] std::span<const source_root> roots() const noexcept;

    // Dense candidate Source universe used by incremental acquisition scans.
    [[nodiscard]] std::size_t source_count() const noexcept;

    // Collects transitive dependents from the committed dependency graph.
    // This is intentionally evaluated before changed Sources replace includes,
    // so every consumer of the previous interface is invalidated.
    [[nodiscard]] status collect_dependents(
        source_id source,
        std::vector<source_id>& output) const noexcept;

    // Records the current net candidate classification at an orchestration
    // measurement boundary. This is telemetry only and is not publication.
    void record_candidate_metrics(metrics_store& metrics) const noexcept;

    [[nodiscard]] status commit() noexcept;

private:
    friend class source_manager;
    friend class graph_build_transaction;
    friend class graph_update;

    source_manager_update(
        source_manager& owner,
        std::uint32_t next_source_id,
        std::uint64_t base_generation) noexcept;

    source_manager* owner = nullptr;
    std::vector<source_record> added_sources;
    std::vector<source_root> root_records;
    std::unordered_map<std::filesystem::path, source_id> added_by_path;
    std::unordered_map<std::uint32_t, std::unique_ptr<source_physical_delta>>
        physical_delta;
    std::unordered_map<std::uint32_t, std::vector<source_id>> include_delta;
    std::unordered_map<std::uint32_t, std::vector<source_id>> prepared_dependents;
    std::vector<source_change> change_records;

    // Dense source_id -> (change_records position + 1) index. Zero means that
    // the Source currently has no net physical change. This keeps acquisition
    // bookkeeping O(1) instead of rescanning all prior changes for every Source.
    std::vector<std::uint32_t> change_positions;

    std::uint32_t next_source_id = 1;
    std::uint64_t base_generation = 0;
    status failure{};
    bool committed = false;
    bool prepared = false;
    bool graph_validated = true;

    // Reserves and constructs every potentially allocating publication
    // structure before the no-fail publish step mutates Source Manager.
    [[nodiscard]] status prepare_publish() noexcept;

    void publish_prepared() noexcept;
    void cancel() noexcept;

    [[nodiscard]] bool contains_for_validation(source_id id) const noexcept;

    [[nodiscard]] std::span<const source_id>
        candidate_includes(source_id id) const noexcept;
};

// Owns the authoritative Source universe for one Project.
// Source Manager maintains Source identity/path mapping, Project roots, physical
// Source state/content, and forward/reverse dependency relationships. It can
// alternatively expose a read-only checkpoint-backed stable view; canonical G
// is outside Source Manager and is not persisted or published here.
class source_manager {
public:
    source_manager();
    ~source_manager();

    // Source Manager is single-owner during one build transaction. Parallel
    // acquisition executes only immutable source_acquire_job values outside
    // Source Manager; candidate/committed state changes are coordinator-owned.
    [[nodiscard]] status initialize() noexcept;

    [[nodiscard]] source_manager_update begin_update() noexcept;

    [[nodiscard]] const source_record* find(source_id id) const noexcept;

    // A committed view remains valid until this Source's committed physical
    // storage is replaced or Source Manager is destroyed.
    [[nodiscard]] status get_view(
        source_id id,
        source_view& output) const noexcept;

    [[nodiscard]] status get_physical_state(
        source_id id,
        source_physical_state& output) const noexcept;

    [[nodiscard]] std::span<const source_root> roots() const noexcept;
    [[nodiscard]] std::span<const source_record> sources() const noexcept;

    [[nodiscard]] std::span<const source_id>
        includes(source_id source) const noexcept;

    [[nodiscard]] std::span<const source_id>
        dependents(source_id source) const noexcept;

    [[nodiscard]] status collect_dependents(
        source_id source,
        std::vector<source_id>& output) const noexcept;

    [[nodiscard]] std::size_t source_count() const noexcept;
    [[nodiscard]] std::size_t root_count() const noexcept;

    [[nodiscard]] status get_path(
        source_id id,
        std::filesystem::path& output) const noexcept;

    [[nodiscard]] status find_by_path(
        const std::filesystem::path& normalized_path,
        source_id& output) const noexcept;

    [[nodiscard]] status get_root(
        std::size_t index,
        source_root& output) const noexcept;

    [[nodiscard]] status get_dependencies(
        source_id source,
        bool reverse,
        std::vector<source_id>& output) const noexcept;

    [[nodiscard]] status save_checkpoint(
        const std::filesystem::path& path) const noexcept;

    [[nodiscard]] status save_checkpoint(
        const std::filesystem::path& path,
        metrics_store* metrics) const noexcept;

    [[nodiscard]] status load_checkpoint(
        const std::filesystem::path& path) noexcept;

    [[nodiscard]] static status strict_validate_checkpoint(
        const std::filesystem::path& path) noexcept;

private:
    friend class source_manager_update;
    friend class graph_build_transaction;
    friend class graph_build_transaction_test_access;

    std::vector<source_record> source_records;
    std::vector<source_root> root_records;
    std::unordered_map<std::filesystem::path, source_id> by_path;
    std::vector<std::unique_ptr<source_physical_storage>> physical_records;
    std::vector<std::vector<source_id>> include_edges;
    std::vector<std::vector<source_id>> dependent_edges;

    std::uint32_t next_source_id = 1;
    std::uint64_t generation = 0;

    // When present, Source Manager serves persisted metadata/dependencies
    // directly from the mapped checkpoint and disallows mutable updates.
    std::unique_ptr<stable_source_manager_view> stable_view;
};

} // namespace cw::server
