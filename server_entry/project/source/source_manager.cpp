#include "source_manager.hpp"
#include "source_manager_persistence.hpp"

#include "file_snapshot.hpp"
#include "../../metrics/source_acquisition_telemetry.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <new>
#include <algorithm>
#include <cassert>
#include <optional>
#include <stdexcept>
#include <utility>
#include <unordered_set>

namespace cw::server {
namespace {
void set_net_change(
    std::vector<source_change>& changes,
    std::vector<std::uint32_t>& positions,
    source_id id,
    std::optional<source_change_kind> kind) {

    if (positions.size() <= id.value()) {
        positions.resize(
            static_cast<std::size_t>(id.value()) + 1,
            0);
    }

    auto& encoded_position = positions[id.value()];

    if (encoded_position != 0) {
        const auto position =
            static_cast<std::size_t>(encoded_position - 1);

        if (kind) {
            changes[position].kind = *kind;
            return;
        }

        const auto last = changes.size() - 1;

        if (position != last) {
            changes[position] = std::move(changes[last]);
            positions[changes[position].source.value()] =
                static_cast<std::uint32_t>(position + 1);
        }

        changes.pop_back();
        encoded_position = 0;
        return;
    }

    if (!kind) {
        return;
    }

    changes.push_back({id, *kind});
    encoded_position =
        static_cast<std::uint32_t>(changes.size());
}

std::optional<source_change_kind> classify_change(
    bool has_committed, const source_physical_state& committed,
    const source_physical_state& candidate) noexcept {
    if (!has_committed)
        return candidate.presence == source_presence::present
                   ? std::optional{source_change_kind::added}
                   : std::nullopt;
    if (committed.presence == source_presence::present &&
        candidate.presence == source_presence::missing)
        return source_change_kind::removed;
    if (committed.presence == source_presence::missing &&
        candidate.presence == source_presence::present)
        return source_change_kind::added;
    if (committed.presence == source_presence::present &&
        candidate.presence == source_presence::present &&
        committed.hash != candidate.hash)
        return source_change_kind::modified;
    return std::nullopt;
}
} // namespace

// Owns the committed immutable bytes and physical metadata of one Source.
// Replacing this object invalidates source_views that referenced its content.
struct source_physical_storage {
    source_physical_state state;
    std::string content;
};

// Holds one candidate physical-state change inside source_manager_update.
// replacement owns new content when bytes changed; observation_only updates
// metadata without replacing the committed immutable content.
struct source_physical_delta {
    source_physical_state state;
    std::unique_ptr<source_physical_storage> replacement;
    bool observation_only = false;
};

source_manager_update::~source_manager_update() = default;
source_manager_update::source_manager_update(source_manager& owner,
                                             std::uint32_t next_source_id,
                                             std::uint64_t base_generation) noexcept
    : owner(&owner), next_source_id(next_source_id), base_generation(base_generation) {
}
source_manager::source_manager() = default;
source_manager::~source_manager() = default;

source_manager_update::source_manager_update(source_manager_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      added_sources(std::move(other.added_sources)),
      root_records(std::move(other.root_records)),
      added_by_path(std::move(other.added_by_path)),
      physical_delta(std::move(other.physical_delta)),
      include_delta(std::move(other.include_delta)),
      prepared_dependents(std::move(other.prepared_dependents)),
      change_records(std::move(other.change_records)),
      change_positions(std::move(other.change_positions)),
      next_source_id(other.next_source_id),
      base_generation(other.base_generation),
      failure(other.failure),
      committed(other.committed),
      prepared(other.prepared),
      graph_validated(other.graph_validated) {
}

status source_manager_update::get_physical_state(
    source_id id,
    source_physical_state& output) const noexcept {

    if (!id || owner == nullptr || committed || prepared) {
        return {status_code::invalid_state};
    }

    const auto candidate =
        physical_delta.find(id.value());

    if (candidate != physical_delta.end()) {
        output = candidate->second->state;
        return {};
    }

    return owner->get_physical_state(id, output);
}

status source_manager_update::get_view(
    source_id id,
    source_view& output) const noexcept {

    output = {};
    if (!id || owner == nullptr || committed || prepared) {
        return {status_code::invalid_state};
    }

    const auto candidate =
        physical_delta.find(id.value());

    if (candidate != physical_delta.end()) {
        const auto& delta = *candidate->second;

        if (delta.state.presence == source_presence::missing) {
            return {status_code::configuration_failed};
        }

        if (delta.replacement) {
            output = {id, delta.replacement->content};
            return {};
        }
    }

    return owner->get_view(id, output);
}

std::span<const source_change> source_manager_update::changes() const noexcept {
    return change_records;
}

std::span<const source_root> source_manager_update::roots() const noexcept {
    return root_records;
}

bool source_manager_update::contains_for_validation(
    source_id id) const noexcept {

    if (!id ||
        owner == nullptr ||
        committed ||
        owner->generation != base_generation) {
        return false;
    }

    if (owner->find(id) != nullptr) {
        return true;
    }

    if (id.value() <= owner->source_records.size()) {
        return false;
    }

    const auto offset =
        id.value() - owner->source_records.size() - 1;

    return
        offset < added_sources.size() &&
        added_sources[offset].id == id;
}

void source_manager_update::record_candidate_metrics(
    metrics_store& metrics) const noexcept {

    if (metrics.mode() == metrics_mode::off) {
        return;
    }

    for (const auto& change : change_records) {
        switch (change.kind) {
        case source_change_kind::added:
            metrics.increment(
                metric_id::source_candidate_added_count);
            break;

        case source_change_kind::modified:
            metrics.increment(
                metric_id::source_candidate_modified_count);
            break;

        case source_change_kind::removed:
            metrics.increment(
                metric_id::source_candidate_removed_count);
            break;
        }
    }

    for (const auto& [id, delta] : physical_delta) {
        (void)id;

        if (delta->observation_only) {
            metrics.increment(
                metric_id::source_candidate_observation_only_count);
        }
    }
}

status source_manager_update::prepare_acquire(
    source_id id,
    source_acquire_job& job) noexcept {

    job = {};

    if (!failure.ok()) {
        return failure;
    }

    if (!id || owner == nullptr || committed || prepared) {
        return {status_code::invalid_state};
    }

    const source_record* record = owner->find(id);

    if (record == nullptr &&
        id.value() > owner->source_records.size()) {
        const auto offset = static_cast<std::size_t>(
            id.value() - owner->source_records.size() - 1);

        if (offset < added_sources.size() &&
            added_sources[offset].id == id) {
            record = &added_sources[offset];
        }
    }

    if (record == nullptr) {
        return {status_code::invalid_state};
    }

    try {
        job.source = id;
        job.path = record->path;
        job.has_committed = owner->get_physical_state(
            id,
            job.committed).ok();

        const auto candidate = physical_delta.find(id.value());

        if (candidate != physical_delta.end()) {
            job.baseline = candidate->second->state;
            job.has_baseline = true;
        }
        else {
            job.has_baseline = owner->get_physical_state(
                id,
                job.baseline).ok();
        }

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        return {status_code::initialization_failed};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::configuration_failed};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status source_manager_update::execute_acquire(
    const source_acquire_job& job,
    source_acquisition_telemetry& metrics,
    source_acquire_result& result) noexcept {

    result = {};
    result.source = job.source;

    if (!job.source || job.path.empty()) {
        return {status_code::invalid_state};
    }

    try {
        std::optional<core::file_snapshot_observation> observation_baseline;

        if (job.has_baseline &&
            job.baseline.presence == source_presence::present) {
            observation_baseline = core::file_snapshot_observation{
                job.baseline.observation.write_time_ticks,
                job.baseline.observation.size};
        }

        metrics.increment(metric_id::source_acquisition_count);
        source_acquisition_timing timing{metrics};
        core::file_snapshot snapshot;

        const auto snapshot_result = core::acquire_file_snapshot(
            job.path,
            observation_baseline,
            snapshot,
            timing);

        if (snapshot_result == core::file_snapshot_result::allocation_failed) {
            metrics.increment(metric_id::source_acquisition_failure_count);
            return {status_code::initialization_failed};
        }

        if (snapshot_result == core::file_snapshot_result::changed_during_read) {
            metrics.increment(metric_id::source_toctou_rejection_count);
            metrics.increment(metric_id::source_acquisition_failure_count);
            return {status_code::configuration_failed};
        }

        if (snapshot_result == core::file_snapshot_result::failed) {
            metrics.increment(metric_id::source_acquisition_failure_count);
            return {status_code::configuration_failed};
        }

        if (snapshot_result == core::file_snapshot_result::unchanged) {
            metrics.increment(metric_id::source_unchanged_fast_path_count);
            result.kind = source_acquire_result_kind::unchanged;
            return {};
        }

        if (snapshot_result == core::file_snapshot_result::missing) {
            result.kind = source_acquire_result_kind::missing;
            return {};
        }

        metrics.increment(metric_id::source_content_read_count);
        metrics.increment(
            metric_id::source_bytes_read,
            static_cast<std::uint64_t>(snapshot.bytes.size()));

        result.kind = source_acquire_result_kind::present;
        result.observation = {
            snapshot.observation.write_time_ticks,
            snapshot.observation.size};
        result.hash = hash_source_content(snapshot.bytes);
        result.content = std::move(snapshot.bytes);

        timing.finish_phase(metric_id::source_sha256_duration);
        return {};
    }
    catch (const std::bad_alloc&) {
        metrics.increment(metric_id::source_acquisition_failure_count);
        return {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        metrics.increment(metric_id::source_acquisition_failure_count);
        return {status_code::initialization_failed};
    }
    catch (const std::filesystem::filesystem_error&) {
        metrics.increment(metric_id::source_acquisition_failure_count);
        return {status_code::configuration_failed};
    }
    catch (...) {
        metrics.increment(metric_id::source_acquisition_failure_count);
        return {status_code::initialization_failed};
    }
}

status source_manager_update::apply_acquire(
    const source_acquire_job& job,
    source_acquire_result&& result,
    source_acquisition_telemetry& metrics) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (!job.source || result.source != job.source ||
        owner == nullptr || committed || prepared) {
        return {status_code::invalid_state};
    }

    try {
        const auto candidate_start = source_telemetry_clock::now();

        if (result.kind == source_acquire_result_kind::unchanged) {
            return {};
        }

        if (result.kind == source_acquire_result_kind::missing) {
            if (!job.has_committed) {
                failure = {status_code::configuration_failed};
                return failure;
            }

            if (job.has_baseline &&
                job.baseline.presence == source_presence::missing) {
                return {};
            }

            auto replacement = std::make_unique<source_physical_storage>();
            replacement->state.presence = source_presence::missing;

            auto delta = std::make_unique<source_physical_delta>();
            delta->state = replacement->state;
            delta->replacement = std::move(replacement);
            physical_delta[job.source.value()] = std::move(delta);

            set_net_change(
                change_records,
                change_positions,
                job.source,
                classify_change(
                    job.has_committed,
                    job.committed,
                    physical_delta[job.source.value()]->state));
        }
        else {
            if (job.has_committed &&
                job.committed.presence == source_presence::present &&
                job.committed.hash == result.hash) {
                auto delta = std::make_unique<source_physical_delta>();
                delta->state = job.committed;
                delta->state.observation = result.observation;
                delta->observation_only = true;
                physical_delta[job.source.value()] = std::move(delta);

                set_net_change(
                    change_records,
                    change_positions,
                    job.source,
                    std::nullopt);
            }
            else if (job.has_baseline &&
                     job.baseline.presence == source_presence::present &&
                     job.baseline.hash == result.hash) {
                const auto existing = physical_delta.find(job.source.value());

                if (existing != physical_delta.end() &&
                    existing->second->replacement) {
                    existing->second->state.observation = result.observation;
                    existing->second->replacement->state.observation = result.observation;
                }
                else {
                    auto delta = std::make_unique<source_physical_delta>();
                    delta->state = job.baseline;
                    delta->state.observation = result.observation;
                    delta->observation_only = true;
                    physical_delta[job.source.value()] = std::move(delta);
                }

                set_net_change(
                    change_records,
                    change_positions,
                    job.source,
                    classify_change(
                        job.has_committed,
                        job.committed,
                        physical_delta[job.source.value()]->state));
            }
            else {
                auto replacement = std::make_unique<source_physical_storage>();
                replacement->state = {
                    source_presence::present,
                    result.observation,
                    result.hash};
                replacement->content = std::move(result.content);

                auto delta = std::make_unique<source_physical_delta>();
                delta->state = replacement->state;
                delta->replacement = std::move(replacement);
                physical_delta[job.source.value()] = std::move(delta);

                set_net_change(
                    change_records,
                    change_positions,
                    job.source,
                    classify_change(
                        job.has_committed,
                        job.committed,
                        physical_delta[job.source.value()]->state));
            }
        }

        if (metrics.mode() == metrics_mode::detailed) {
            metrics.record_duration(
                metric_id::source_candidate_update_duration,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    source_telemetry_clock::now() - candidate_start));
        }

        return {};
    }
    catch (const std::bad_alloc&) {
        failure = {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        failure = {status_code::initialization_failed};
    }
    catch (...) {
        failure = {status_code::initialization_failed};
    }

    return failure;
}

status source_manager_update::acquire(
    source_id id,
    source_acquisition_telemetry& metrics) noexcept {

    source_acquire_job job;
    auto result = prepare_acquire(id, job);

    if (!result.ok()) {
        return result;
    }

    source_acquire_result acquired;
    result = execute_acquire(job, metrics, acquired);

    if (!result.ok()) {
        if (failure.ok()) {
            failure = result;
        }
        return failure;
    }

    return apply_acquire(job, std::move(acquired), metrics);
}

status source_manager_update::add(const std::filesystem::path& path,
                                  project_item_role role) noexcept {
    source_id ignored;
    return resolve(path, role, ignored);
}

status source_manager_update::resolve(
    const std::filesystem::path& path,
    project_item_role role,
    source_id& output) noexcept {

    output = {};
    if (!failure.ok()) {
        return failure;
    }

    if (committed || prepared || owner == nullptr) {
        return {status_code::invalid_state};
    }

    if (path.empty() || role == project_item_role::project) {
        failure = {status_code::configuration_failed};
        return failure;
    }

    try {
        const auto& normalized = path;
        const auto added = added_by_path.find(normalized);
        source_id id;

        if (added != added_by_path.end()) {
            id = added->second;
        }
        else {
            const auto committed_source =
                owner->by_path.find(normalized);

            if (committed_source != owner->by_path.end()) {
                id = committed_source->second;
            }
            else {
                if (next_source_id == 0) {
                    failure = {
                        status_code::initialization_failed
                    };

                    return failure;
                }

                id = source_id{next_source_id};

                const auto [position, inserted] =
                    added_by_path.emplace(normalized, id);

                if (!inserted) {
                    id = position->second;
                }
                else {
                    try {
                        added_sources.push_back({
                            id,
                            normalized
                        });
                    }
                    catch (...) {
                        added_by_path.erase(position);
                        throw;
                    }

                    ++next_source_id;
                }
            }
        }

        if (role == project_item_role::source ||
            role == project_item_role::type) {
            root_records.push_back({id, role});
        }

        output = id;
        return {};
    }
    catch (const std::bad_alloc&) {
        failure = {status_code::initialization_failed};
    }
    catch (const std::length_error&) {
        failure = {status_code::initialization_failed};
    }
    catch (const std::filesystem::filesystem_error&) {
        failure = {status_code::configuration_failed};
    }

    return failure;
}

status source_manager_update::resolve_include(
    const std::filesystem::path& path,
    source_id& output) noexcept {

    output = {};

    std::error_code error;

    if (!std::filesystem::is_regular_file(path, error) || error) {
            if (failure.ok()) {
            failure = {status_code::configuration_failed};
        }

        return failure;
    }

    const auto root_count = root_records.size();
    const auto result =
        resolve(path, project_item_role::source, output);

    if (result.ok()) {
        root_records.resize(root_count);
    }

    return result;
}

status source_manager_update::resolve_include(
    source_id including_source,
    std::string_view relative_path,
    source_id& output) noexcept {

    output = {};
    std::filesystem::path including_path;

    try {
        {
                    if (!contains_for_validation(including_source) ||
                relative_path.empty()) {
                return {status_code::invalid_state};
            }

            const source_record* record =
                owner->find(including_source);

            if (!record &&
                including_source.value() >
                    owner->source_records.size()) {
                const auto offset =
                    including_source.value() -
                    owner->source_records.size() -
                    1;

                if (offset < added_sources.size()) {
                    record = &added_sources[offset];
                }
            }

            if (!record) {
                return {status_code::invalid_state};
            }

            including_path = record->path;
        }

        const auto path =
            (including_path.parent_path() /
             std::filesystem::path{
                 std::string{relative_path}})
                .lexically_normal();

        return resolve_include(path, output);
    }
    catch (const std::bad_alloc&) {
        if (failure.ok()) {
            failure = {status_code::initialization_failed};
        }
        return failure;
    }
    catch (const std::length_error&) {
        if (failure.ok()) {
            failure = {status_code::initialization_failed};
        }
        return failure;
    }
    catch (const std::filesystem::filesystem_error&) {
        if (failure.ok()) {
            failure = {status_code::configuration_failed};
        }
        return failure;
    }
}

std::span<const source_id> source_manager_update::candidate_includes(source_id id) const noexcept {
    const auto changed = include_delta.find(id.value());
    if (changed != include_delta.end()) return changed->second;
    return owner ? owner->includes(id) : std::span<const source_id>{};
}

std::span<const source_id> source_manager_update::includes(source_id id) const noexcept {
    return candidate_includes(id);
}

status source_manager_update::set_includes(
    source_id source,
    const std::span<const source_id> includes) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (committed ||
        prepared ||
        !contains_for_validation(source)) {
        return {status_code::invalid_state};
    }

    try {
        std::vector<source_id> unique;
        unique.reserve(includes.size());

        for (const auto included : includes) {
            if (!contains_for_validation(included)) {
                failure = {
                    status_code::configuration_failed
                };

                return failure;
            }

            if (std::find(
                    unique.begin(),
                    unique.end(),
                    included) == unique.end()) {
                unique.push_back(included);
            }
        }

        include_delta[source.value()] =
            std::move(unique);

        graph_validated = false;
        return {};
    }
    catch (...) {
        failure = {
            status_code::initialization_failed
        };

        return failure;
    }
}

// Validates the complete candidate include graph before publication.
// The iterative candidate overlay is authoritative for changed include lists.
status source_manager_update::validate_source_graph(
    operation_id operation, diagnostic_buffer& diagnostics) noexcept {
    if (!failure.ok()) return failure;
    try {
        const auto count = owner->source_records.size() + added_sources.size();
        std::vector<std::uint8_t> state(count + 1);
        const auto visit = [&](auto&& self, source_id source) -> bool {
            auto& value = state[source.value()];
            if (value == 1) return false;
            if (value == 2) return true;
            value = 1;
            for (const auto dependency : candidate_includes(source))
                if (!self(self, dependency)) return false;
            value = 2;
            return true;
        };
        for (std::uint32_t value = 1; value <= count; ++value)
            if (!visit(visit, source_id{value})) {
                try {
                    diagnostics.emit({diagnostics::source_include_cycle.id,
                        diagnostics::source_include_cycle.default_severity,
                        operation, {source_id{value}, 0, 0}, {}});
                }
                catch (...) {
                    return failure = {status_code::initialization_failed};
                }
                return failure = {status_code::configuration_failed};
            }
        graph_validated = true;
        return {};
    }
    catch (...) {
        return failure = {status_code::initialization_failed};
    }
}

status source_manager_update::commit() noexcept {
    const auto result = prepare_publish();
    if (!result.ok()) return result;
    publish_prepared();
    return {};
}

status source_manager::initialize() noexcept { return {}; }
source_manager_update source_manager::begin_update() noexcept {
    auto update = source_manager_update{*this, next_source_id, generation};
    if (stable_view) update.owner = nullptr;
    return update;
}

const source_record* source_manager::find(source_id id) const noexcept {
    if (!id || id.value() > source_records.size()) return nullptr;
    const auto& candidate = source_records[id.value() - 1];
    return candidate.id == id ? &candidate : nullptr;
}

status source_manager::get_physical_state(source_id id,
                                          source_physical_state& output) const noexcept {
    if (stable_view) return stable_view->physical(id, output);
    if (!id || id.value() > physical_records.size() || !physical_records[id.value() - 1])
        return {status_code::invalid_state};
    output = physical_records[id.value() - 1]->state;
    return {};
}

status source_manager::get_view(source_id id, source_view& output) const noexcept {
    output = {};
    if (stable_view) return {status_code::not_available};
    if (!id || id.value() > physical_records.size() || !physical_records[id.value() - 1])
        return {status_code::invalid_state};
    const auto& physical = *physical_records[id.value() - 1];
    if (physical.state.presence == source_presence::missing)
        return {status_code::configuration_failed};
    output = {id, physical.content};
    return {};
}

std::span<const source_root> source_manager::roots() const noexcept { return root_records; }
std::span<const source_record> source_manager::sources() const noexcept { return source_records; }
std::span<const source_id> source_manager::includes(source_id source) const noexcept {
    return source && source.value() <= include_edges.size()
        ? std::span<const source_id>{include_edges[source.value() - 1]}
        : std::span<const source_id>{};
}
std::span<const source_id> source_manager::dependents(source_id source) const noexcept {
    return source && source.value() <= dependent_edges.size()
        ? std::span<const source_id>{dependent_edges[source.value() - 1]}
        : std::span<const source_id>{};
}

status source_manager::collect_dependents(source_id source,
                                          std::vector<source_id>& output) const noexcept {
    if (stable_view) return stable_view->dependencies(source, true, output);
    output.clear();
    if (!find(source)) return {status_code::invalid_state};
    try {
        std::vector<bool> visited(source_records.size() + 1);
        visited[source.value()] = true;
        for (const auto dependent : dependents(source)) {
            visited[dependent.value()] = true;
            output.push_back(dependent);
        }
        for (std::size_t index = 0; index < output.size(); ++index) {
            for (const auto dependent : dependents(output[index])) {
                if (visited[dependent.value()]) continue;
                visited[dependent.value()] = true;
                output.push_back(dependent);
            }
        }
        return {};
    }
    catch (...) {
        output.clear();
        return {status_code::initialization_failed};
    }
}

std::size_t source_manager::source_count() const noexcept {
    return stable_view ? stable_view->source_count() : source_records.size();
}

std::size_t source_manager::root_count() const noexcept {
    return stable_view ? stable_view->root_count() : root_records.size();
}

status source_manager::get_path(source_id id, std::filesystem::path& output) const noexcept {
    output.clear();
    if (stable_view) return stable_view->path(id, output);
    const auto* record = find(id);
    if (!record) return {status_code::invalid_state};
    try { output = record->path; return {}; }
    catch (...) { output.clear(); return {status_code::initialization_failed}; }
}

status source_manager::find_by_path(const std::filesystem::path& normalized_path,
                                    source_id& output) const noexcept {
    output = {};
    if (stable_view) return stable_view->find(normalized_path, output);
    const auto found = by_path.find(normalized_path);
    if (found == by_path.end()) return {status_code::not_available};
    output = found->second;
    return {};
}

status source_manager::get_root(std::size_t index, source_root& output) const noexcept {
    output = {};
    if (stable_view) return stable_view->root(index, output);
    if (index >= root_records.size()) return {status_code::invalid_state};
    output = root_records[index];
    return {};
}

status source_manager::get_dependencies(source_id source, bool reverse,
                                        std::vector<source_id>& output) const noexcept {
    if (stable_view) return stable_view->dependencies(source, reverse, output);
    output.clear();
    if (!find(source)) return {status_code::invalid_state};
    try {
        const auto values = reverse ? dependents(source) : includes(source);
        output.assign(values.begin(), values.end());
        return {};
    }
    catch (...) { output.clear(); return {status_code::initialization_failed}; }
}

status source_manager::save_checkpoint(const std::filesystem::path& path) const noexcept {
    return save_checkpoint(path, nullptr);
}

status source_manager::save_checkpoint(
    const std::filesystem::path& path, metrics_store* metrics) const noexcept {
    if (stable_view) return {status_code::invalid_state};
    return write_source_manager_checkpoint(*this, path, metrics);
}

status source_manager::load_checkpoint(const std::filesystem::path& path) noexcept {
    if (stable_view || !source_records.empty() || !root_records.empty() || !by_path.empty() ||
        !physical_records.empty() || !include_edges.empty() || !dependent_edges.empty())
        return {status_code::invalid_state};
    try {
        auto candidate = std::make_unique<stable_source_manager_view>();
        const auto result = candidate->open(path);
        if (!result.ok()) return result;
        stable_view = std::move(candidate);
        return {};
    }
    catch (...) { return {status_code::initialization_failed}; }
}

status source_manager::strict_validate_checkpoint(
    const std::filesystem::path& path) noexcept {
    return strict_validate_source_manager_checkpoint(path);
}

// Completes every potentially allocating preparation step before Source
// Manager mutation begins, including reverse-dependency reconstruction.
status source_manager_update::prepare_publish() noexcept {
    if (!failure.ok()) return failure;
    if (committed || prepared || owner == nullptr)
        return {status_code::invalid_state};
    if (base_generation != owner->generation)
        return failure = {status_code::invalid_state};
    if (!graph_validated)
        return failure = {status_code::configuration_failed};
    try {
        owner->source_records.reserve(owner->source_records.size() + added_sources.size());
        owner->by_path.reserve(owner->by_path.size() + added_by_path.size());
        owner->physical_records.reserve(owner->source_records.size() + added_sources.size());
        owner->include_edges.reserve(owner->source_records.size() + added_sources.size());
        owner->dependent_edges.reserve(owner->source_records.size() + added_sources.size());
        prepared_dependents.clear();
        for (const auto& [parent_value, replacement] : include_delta) {
            const source_id parent{parent_value};
            const auto old = owner->includes(parent);
            for (const auto child : old) {
                if (std::find(replacement.begin(), replacement.end(), child) == replacement.end()) {
                    auto [position, inserted] = prepared_dependents.try_emplace(
                        child.value(), owner->dependents(child).begin(), owner->dependents(child).end());
                    auto& values = position->second;
                    values.erase(std::remove(values.begin(), values.end(), parent), values.end());
                }
            }
            for (const auto child : replacement) {
                if (std::find(old.begin(), old.end(), child) == old.end()) {
                    auto [position, inserted] = prepared_dependents.try_emplace(
                        child.value(), owner->dependents(child).begin(), owner->dependents(child).end());
                    auto& values = position->second;
                    if (std::find(values.begin(), values.end(), parent) == values.end())
                        values.push_back(parent);
                }
            }
        }
    }
    catch (const std::bad_alloc&)
    { return failure = {status_code::initialization_failed}; }
    catch (const std::length_error&)
    { return failure = {status_code::initialization_failed}; }
    prepared = true;
    return {};
}

// Applies the prepared candidate without further allocation-sensitive setup.
// After publication generation advances and the update becomes permanently closed.
void source_manager_update::publish_prepared() noexcept {
    assert(prepared && !committed && owner != nullptr);
    owner->physical_records.resize(owner->source_records.size() + added_sources.size());
    owner->include_edges.resize(owner->source_records.size() + added_sources.size());
    owner->dependent_edges.resize(owner->source_records.size() + added_sources.size());
    for (auto& source : added_sources) owner->source_records.push_back(std::move(source));
    owner->by_path.merge(added_by_path);
    owner->root_records.swap(root_records);
    for (auto& [value, includes] : include_delta)
        owner->include_edges[value - 1].swap(includes);
    for (auto& [value, dependents] : prepared_dependents)
        owner->dependent_edges[value - 1].swap(dependents);
    for (auto& [value, delta] : physical_delta) {
        auto& committed_storage = owner->physical_records[value - 1];
        if (delta->observation_only) {
            if (committed_storage) {
                committed_storage->state.observation = delta->state.observation;
            }
        }
        else {
            committed_storage = std::move(delta->replacement);
        }
    }
    owner->next_source_id = next_source_id;
    ++owner->generation;
    committed = true;
}

void source_manager_update::cancel() noexcept {
    if (!committed) {
        prepared = true;
        failure = {status_code::invalid_state};
    }
}
} // namespace cw::server
