#include "source_manager.hpp"
#include "source_manager_persistence.hpp"

#include "../../core/filesystem/file_snapshot.hpp"
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

namespace cw::server
{
namespace
{
void set_net_change(std::vector<source_change>& changes, source_id id,
                    std::optional<source_change_kind> kind)
{
    for (auto position = changes.begin(); position != changes.end(); ++position)
    {
        if (position->source != id) continue;
        if (kind) position->kind = *kind;
        else changes.erase(position);
        return;
    }
    if (kind) changes.push_back({id, *kind});
}

std::optional<source_change_kind> classify_change(
    bool has_committed, const source_physical_state& committed,
    const source_physical_state& candidate) noexcept
{
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

struct source_physical_storage
{
    source_physical_state state;
    std::string content;
};

struct source_physical_delta
{
    source_physical_state state;
    std::unique_ptr<source_physical_storage> replacement;
    bool observation_only = false;
};

source_manager_update::~source_manager_update() = default;
source_manager_update::source_manager_update(source_manager& owner,
                                             std::uint32_t next_source_id,
                                             std::uint64_t base_generation) noexcept
    : owner_(&owner), next_source_id_(next_source_id), base_generation_(base_generation)
{
}
source_manager::source_manager() = default;
source_manager::~source_manager() = default;

source_manager_update::source_manager_update(source_manager_update&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      added_sources_(std::move(other.added_sources_)),
      roots_(std::move(other.roots_)),
      added_by_path_(std::move(other.added_by_path_)),
      physical_delta_(std::move(other.physical_delta_)),
      include_delta_(std::move(other.include_delta_)),
      prepared_dependents_(std::move(other.prepared_dependents_)),
      changes_(std::move(other.changes_)),
      next_source_id_(other.next_source_id_),
      base_generation_(other.base_generation_),
      failure_(other.failure_),
      committed_(other.committed_),
      prepared_(other.prepared_),
      graph_validated_(other.graph_validated_)
{
}

status source_manager_update::get_physical_state(
    source_id id, source_physical_state& output) const noexcept
{
    if (!id || owner_ == nullptr || committed_ || prepared_) return {status_code::invalid_state};
    const auto candidate = physical_delta_.find(id.value());
    if (candidate != physical_delta_.end())
    {
        output = candidate->second->state;
        return {};
    }
    return owner_->get_physical_state(id, output);
}

status source_manager_update::get_view(source_id id, source_view& output) const noexcept
{
    output = {};
    if (!id || owner_ == nullptr || committed_ || prepared_) return {status_code::invalid_state};
    const auto candidate = physical_delta_.find(id.value());
    if (candidate != physical_delta_.end())
    {
        const auto& delta = *candidate->second;
        if (delta.state.presence == source_presence::missing)
            return {status_code::configuration_failed};
        if (delta.replacement)
        {
            output = {id, delta.replacement->content};
            return {};
        }
    }
    return owner_->get_view(id, output);
}

std::span<const source_change> source_manager_update::changes() const noexcept
{
    return changes_;
}

std::span<const source_root> source_manager_update::roots() const noexcept
{
    return roots_;
}

bool source_manager_update::contains_for_validation(source_id id) const noexcept
{
    if (!id || owner_ == nullptr || committed_ || owner_->generation_ != base_generation_)
        return false;
    if (owner_->find(id) != nullptr) return true;
    if (id.value() <= owner_->sources_.size()) return false;
    const auto offset = id.value() - owner_->sources_.size() - 1;
    return offset < added_sources_.size() && added_sources_[offset].id == id;
}

void source_manager_update::record_candidate_metrics(metrics_store& metrics) const noexcept
{
    if (metrics.mode() == metrics_mode::off) return;
    for (const auto& change : changes_)
    {
        switch (change.kind)
        {
        case source_change_kind::added:
            metrics.increment(metric_id::source_candidate_added_count);
            break;
        case source_change_kind::modified:
            metrics.increment(metric_id::source_candidate_modified_count);
            break;
        case source_change_kind::removed:
            metrics.increment(metric_id::source_candidate_removed_count);
            break;
        }
    }
    for (const auto& [id, delta] : physical_delta_)
    {
        (void)id;
        if (delta->observation_only)
            metrics.increment(metric_id::source_candidate_observation_only_count);
    }
}

status source_manager_update::acquire(
    source_id id, source_acquisition_telemetry& metrics) noexcept
{
    if (!failure_.ok()) return failure_;
    if (committed_ || prepared_ || owner_ == nullptr || !id)
        return {status_code::invalid_state};

    const source_record* record = owner_->find(id);
    if (record == nullptr && id.value() > owner_->sources_.size())
    {
        const auto offset = static_cast<std::size_t>(id.value() - owner_->sources_.size() - 1);
        if (offset < added_sources_.size() && added_sources_[offset].id == id)
            record = &added_sources_[offset];
    }
    if (record == nullptr) return {status_code::invalid_state};

    bool acquisition_attempted = false;
    try
    {
        source_physical_state committed_state;
        const bool has_committed = owner_->get_physical_state(id, committed_state).ok();
        source_physical_state baseline;
        const bool has_baseline = get_physical_state(id, baseline).ok();
        std::optional<core::file_snapshot_observation> observation_baseline;
        if (has_baseline && baseline.presence == source_presence::present)
            observation_baseline = core::file_snapshot_observation{
                baseline.observation.write_time_ticks, baseline.observation.size};
        acquisition_attempted = true;
        metrics.increment(metric_id::source_acquisition_count);
        source_acquisition_timing timing{metrics};
        core::file_snapshot snapshot;
        const auto snapshot_result = core::acquire_file_snapshot(
            record->path, observation_baseline, snapshot, timing);
        if (snapshot_result == core::file_snapshot_result::missing)
        {
            if (!has_committed)
            {
                failure_ = {status_code::configuration_failed};
                metrics.increment(metric_id::source_acquisition_failure_count);
                return failure_;
            }
            if (baseline.presence == source_presence::missing) return {};
            auto replacement = std::make_unique<source_physical_storage>();
            replacement->state.presence = source_presence::missing;
            auto delta = std::make_unique<source_physical_delta>();
            delta->state = replacement->state;
            delta->replacement = std::move(replacement);
            physical_delta_[id.value()] = std::move(delta);
            set_net_change(changes_, id,
                           classify_change(has_committed, committed_state,
                                           physical_delta_[id.value()]->state));
            return {};
        }
        if (snapshot_result == core::file_snapshot_result::allocation_failed)
        {
            failure_ = {status_code::initialization_failed};
            metrics.increment(metric_id::source_acquisition_failure_count);
            return failure_;
        }
        if (snapshot_result == core::file_snapshot_result::changed_during_read)
        {
            failure_ = {status_code::configuration_failed};
            metrics.increment(metric_id::source_toctou_rejection_count);
            metrics.increment(metric_id::source_acquisition_failure_count);
            return failure_;
        }
        if (snapshot_result == core::file_snapshot_result::failed)
        {
            failure_ = {status_code::configuration_failed};
            metrics.increment(metric_id::source_acquisition_failure_count);
            return failure_;
        }
        // Equal observation is the SM2 change-detection fast path, not proof
        // that current filesystem bytes are equal. SHA-256 remains exact for
        // the immutable content acquired and stored by Source Manager.
        if (snapshot_result == core::file_snapshot_result::unchanged)
        {
            metrics.increment(metric_id::source_unchanged_fast_path_count);
            return {};
        }
        metrics.increment(metric_id::source_content_read_count);
        metrics.increment(metric_id::source_bytes_read,
                          static_cast<std::uint64_t>(snapshot.bytes.size()));
        const source_observation observation{snapshot.observation.write_time_ticks,
                                             snapshot.observation.size};
        source_content_hash hash;
        hash = hash_source_content(snapshot.bytes);
        timing.finish_phase(metric_id::source_sha256_duration);
        if (has_committed && committed_state.presence == source_presence::present &&
            committed_state.hash == hash)
        {
            auto delta = std::make_unique<source_physical_delta>();
            delta->state = committed_state;
            delta->state.observation = observation;
            delta->observation_only = true;
            physical_delta_[id.value()] = std::move(delta);
            set_net_change(changes_, id, std::nullopt);
            timing.finish_phase(metric_id::source_candidate_update_duration);
            return {};
        }
        if (has_baseline && baseline.presence == source_presence::present && baseline.hash == hash)
        {
            const auto existing = physical_delta_.find(id.value());
            if (existing != physical_delta_.end() && existing->second->replacement)
            {
                existing->second->state.observation = observation;
                existing->second->replacement->state.observation = observation;
            }
            else
            {
                auto delta = std::make_unique<source_physical_delta>();
                delta->state = baseline;
                delta->state.observation = observation;
                delta->observation_only = true;
                physical_delta_[id.value()] = std::move(delta);
            }
            set_net_change(changes_, id,
                           classify_change(has_committed, committed_state,
                                           physical_delta_[id.value()]->state));
            timing.finish_phase(metric_id::source_candidate_update_duration);
            return {};
        }

        auto replacement = std::make_unique<source_physical_storage>();
        replacement->state = {source_presence::present, observation, hash};
        replacement->content = std::move(snapshot.bytes);
        auto delta = std::make_unique<source_physical_delta>();
        delta->state = replacement->state;
        delta->replacement = std::move(replacement);
        physical_delta_[id.value()] = std::move(delta);
        set_net_change(changes_, id,
                       classify_change(has_committed, committed_state,
                                       physical_delta_[id.value()]->state));
        timing.finish_phase(metric_id::source_candidate_update_duration);
        return {};
    }
    catch (const std::bad_alloc&) { failure_ = {status_code::initialization_failed}; }
    catch (const std::length_error&) { failure_ = {status_code::initialization_failed}; }
    catch (const std::filesystem::filesystem_error&)
    { failure_ = {status_code::configuration_failed}; }
    if (acquisition_attempted)
        metrics.increment(metric_id::source_acquisition_failure_count);
    return failure_;
}

status source_manager_update::add(const std::filesystem::path& path,
                                  project_item_role role) noexcept
{
    source_id ignored;
    return resolve(path, role, ignored);
}

status source_manager_update::resolve(const std::filesystem::path& path,
                                      project_item_role role,
                                      source_id& output) noexcept
{
    output = {};
    if (!failure_.ok()) return failure_;
    if (committed_ || prepared_ || owner_ == nullptr) return {status_code::invalid_state};
    if (path.empty() || role == project_item_role::project)
    {
        failure_ = {status_code::configuration_failed};
        return failure_;
    }
    try
    {
        const auto& normalized = path;
        const auto added = added_by_path_.find(normalized);
        source_id id;
        if (added != added_by_path_.end()) id = added->second;
        else
        {
            const auto committed = owner_->by_path_.find(normalized);
            if (committed != owner_->by_path_.end()) id = committed->second;
            else
            {
                if (next_source_id_ == 0)
                {
                    failure_ = {status_code::initialization_failed};
                    return failure_;
                }
                id = source_id{next_source_id_};
                const auto [position, inserted] = added_by_path_.emplace(normalized, id);
                if (!inserted) id = position->second;
                else
                {
                    try { added_sources_.push_back({id, normalized}); }
                    catch (const std::bad_alloc&) { added_by_path_.erase(position); throw; }
                    catch (const std::length_error&) { added_by_path_.erase(position); throw; }
                    ++next_source_id_;
                }
            }
        }
        if (role == project_item_role::source || role == project_item_role::type)
            roots_.push_back({id, role});
        output = id;
        return {};
    }
    catch (const std::bad_alloc&) { failure_ = {status_code::initialization_failed}; }
    catch (const std::length_error&) { failure_ = {status_code::initialization_failed}; }
    catch (const std::filesystem::filesystem_error&)
    { failure_ = {status_code::configuration_failed}; }
    return failure_;
}

status source_manager_update::resolve_include(const std::filesystem::path& path,
                                              source_id& output) noexcept
{
    output = {};
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return failure_ = {status_code::configuration_failed};
    const auto root_count = roots_.size();
    const auto result = resolve(path, project_item_role::source, output);
    if (result.ok()) roots_.resize(root_count);
    return result;
}

status source_manager_update::resolve_include(
    source_id including_source, std::string_view relative_path,
    source_id& output) noexcept
{
    output = {};
    if (!contains_for_validation(including_source) || relative_path.empty())
        return {status_code::invalid_state};
    try
    {
        const source_record* record = owner_->find(including_source);
        if (!record && including_source.value() > owner_->sources_.size())
        {
            const auto offset = including_source.value() - owner_->sources_.size() - 1;
            if (offset < added_sources_.size()) record = &added_sources_[offset];
        }
        if (!record) return {status_code::invalid_state};
        const auto path = (record->path.parent_path() /
            std::filesystem::path{std::string{relative_path}}).lexically_normal();
        return resolve_include(path, output);
    }
    catch (const std::bad_alloc&)
    { return failure_ = {status_code::initialization_failed}; }
    catch (const std::length_error&)
    { return failure_ = {status_code::initialization_failed}; }
    catch (const std::filesystem::filesystem_error&)
    { return failure_ = {status_code::configuration_failed}; }
}

std::span<const source_id> source_manager_update::candidate_includes(source_id id) const noexcept
{
    const auto changed = include_delta_.find(id.value());
    if (changed != include_delta_.end()) return changed->second;
    return owner_ ? owner_->includes(id) : std::span<const source_id>{};
}

std::span<const source_id> source_manager_update::includes(source_id id) const noexcept
{
    return candidate_includes(id);
}

status source_manager_update::set_includes(
    source_id source, const std::span<const source_id> includes) noexcept
{
    if (!failure_.ok()) return failure_;
    if (committed_ || prepared_ || !contains_for_validation(source))
        return {status_code::invalid_state};
    try
    {
        std::vector<source_id> unique;
        unique.reserve(includes.size());
        for (const auto included : includes)
        {
            if (!contains_for_validation(included))
                return failure_ = {status_code::configuration_failed};
            if (std::find(unique.begin(), unique.end(), included) == unique.end())
                unique.push_back(included);
        }
        include_delta_[source.value()] = std::move(unique);
        graph_validated_ = false;
        return {};
    }
    catch (...)
    {
        return failure_ = {status_code::initialization_failed};
    }
}

status source_manager_update::validate_source_graph(
    operation_id operation, diagnostic_buffer& diagnostics) noexcept
{
    if (!failure_.ok()) return failure_;
    try
    {
        const auto count = owner_->sources_.size() + added_sources_.size();
        std::vector<std::uint8_t> state(count + 1);
        const auto visit = [&](auto&& self, source_id source) -> bool
        {
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
            if (!visit(visit, source_id{value}))
            {
                try
                {
                    diagnostics.emit({diagnostics::source_include_cycle.id,
                        diagnostics::source_include_cycle.default_severity,
                        operation, {source_id{value}, 0, 0}, {}});
                }
                catch (...)
                {
                    return failure_ = {status_code::initialization_failed};
                }
                return failure_ = {status_code::configuration_failed};
            }
        graph_validated_ = true;
        return {};
    }
    catch (...)
    {
        return failure_ = {status_code::initialization_failed};
    }
}

status source_manager_update::commit() noexcept
{
    const auto result = prepare_publish();
    if (!result.ok()) return result;
    publish_prepared();
    return {};
}

status source_manager::initialize() noexcept { return {}; }
source_manager_update source_manager::begin_update() noexcept
{
    auto update = source_manager_update{*this, next_source_id_, generation_};
    if (stable_) update.owner_ = nullptr;
    return update;
}

const source_record* source_manager::find(source_id id) const noexcept
{
    if (!id || id.value() > sources_.size()) return nullptr;
    const auto& candidate = sources_[id.value() - 1];
    return candidate.id == id ? &candidate : nullptr;
}

status source_manager::get_physical_state(source_id id,
                                          source_physical_state& output) const noexcept
{
    if (stable_) return stable_->physical(id, output);
    if (!id || id.value() > physical_.size() || !physical_[id.value() - 1])
        return {status_code::invalid_state};
    output = physical_[id.value() - 1]->state;
    return {};
}

status source_manager::get_view(source_id id, source_view& output) const noexcept
{
    output = {};
    if (stable_) return {status_code::not_available};
    if (!id || id.value() > physical_.size() || !physical_[id.value() - 1])
        return {status_code::invalid_state};
    const auto& physical = *physical_[id.value() - 1];
    if (physical.state.presence == source_presence::missing)
        return {status_code::configuration_failed};
    output = {id, physical.content};
    return {};
}

std::span<const source_root> source_manager::roots() const noexcept { return roots_; }
std::span<const source_record> source_manager::sources() const noexcept { return sources_; }
std::span<const source_id> source_manager::includes(source_id source) const noexcept
{
    return source && source.value() <= includes_.size()
        ? std::span<const source_id>{includes_[source.value() - 1]}
        : std::span<const source_id>{};
}
std::span<const source_id> source_manager::dependents(source_id source) const noexcept
{
    return source && source.value() <= dependents_.size()
        ? std::span<const source_id>{dependents_[source.value() - 1]}
        : std::span<const source_id>{};
}

status source_manager::collect_dependents(source_id source,
                                          std::vector<source_id>& output) const noexcept
{
    if (stable_) return stable_->dependencies(source, true, output);
    output.clear();
    if (!find(source)) return {status_code::invalid_state};
    try
    {
        std::vector<bool> visited(sources_.size() + 1);
        visited[source.value()] = true;
        for (const auto dependent : dependents(source))
        {
            visited[dependent.value()] = true;
            output.push_back(dependent);
        }
        for (std::size_t index = 0; index < output.size(); ++index)
        {
            for (const auto dependent : dependents(output[index]))
            {
                if (visited[dependent.value()]) continue;
                visited[dependent.value()] = true;
                output.push_back(dependent);
            }
        }
        return {};
    }
    catch (...)
    {
        output.clear();
        return {status_code::initialization_failed};
    }
}

std::size_t source_manager::source_count() const noexcept
{
    return stable_ ? stable_->source_count() : sources_.size();
}

std::size_t source_manager::root_count() const noexcept
{
    return stable_ ? stable_->root_count() : roots_.size();
}

status source_manager::get_path(source_id id, std::filesystem::path& output) const noexcept
{
    output.clear();
    if (stable_) return stable_->path(id, output);
    const auto* record = find(id);
    if (!record) return {status_code::invalid_state};
    try { output = record->path; return {}; }
    catch (...) { output.clear(); return {status_code::initialization_failed}; }
}

status source_manager::find_by_path(const std::filesystem::path& normalized_path,
                                    source_id& output) const noexcept
{
    output = {};
    if (stable_) return stable_->find(normalized_path, output);
    const auto found = by_path_.find(normalized_path);
    if (found == by_path_.end()) return {status_code::not_available};
    output = found->second;
    return {};
}

status source_manager::get_root(std::size_t index, source_root& output) const noexcept
{
    output = {};
    if (stable_) return stable_->root(index, output);
    if (index >= roots_.size()) return {status_code::invalid_state};
    output = roots_[index];
    return {};
}

status source_manager::get_dependencies(source_id source, bool reverse,
                                        std::vector<source_id>& output) const noexcept
{
    if (stable_) return stable_->dependencies(source, reverse, output);
    output.clear();
    if (!find(source)) return {status_code::invalid_state};
    try
    {
        const auto values = reverse ? dependents(source) : includes(source);
        output.assign(values.begin(), values.end());
        return {};
    }
    catch (...) { output.clear(); return {status_code::initialization_failed}; }
}

status source_manager::save_checkpoint(const std::filesystem::path& path) const noexcept
{
    return save_checkpoint(path, nullptr);
}

status source_manager::save_checkpoint(
    const std::filesystem::path& path, metrics_store* metrics) const noexcept
{
    if (stable_) return {status_code::invalid_state};
    return write_source_manager_checkpoint(*this, path, metrics);
}

status source_manager::load_checkpoint(const std::filesystem::path& path) noexcept
{
    if (stable_ || !sources_.empty() || !roots_.empty() || !by_path_.empty() ||
        !physical_.empty() || !includes_.empty() || !dependents_.empty())
        return {status_code::invalid_state};
    try
    {
        auto candidate = std::make_unique<stable_source_manager_view>();
        const auto result = candidate->open(path);
        if (!result.ok()) return result;
        stable_ = std::move(candidate);
        return {};
    }
    catch (...) { return {status_code::initialization_failed}; }
}

status source_manager::strict_validate_checkpoint(
    const std::filesystem::path& path) noexcept
{
    return strict_validate_source_manager_checkpoint(path);
}

status source_manager_update::prepare_publish() noexcept
{
    if (!failure_.ok()) return failure_;
    if (committed_ || prepared_ || owner_ == nullptr)
        return {status_code::invalid_state};
    if (base_generation_ != owner_->generation_)
        return failure_ = {status_code::invalid_state};
    if (!graph_validated_)
        return failure_ = {status_code::configuration_failed};
    try
    {
        owner_->sources_.reserve(owner_->sources_.size() + added_sources_.size());
        owner_->by_path_.reserve(owner_->by_path_.size() + added_by_path_.size());
        owner_->physical_.reserve(owner_->sources_.size() + added_sources_.size());
        owner_->includes_.reserve(owner_->sources_.size() + added_sources_.size());
        owner_->dependents_.reserve(owner_->sources_.size() + added_sources_.size());
        prepared_dependents_.clear();
        for (const auto& [parent_value, replacement] : include_delta_)
        {
            const source_id parent{parent_value};
            const auto old = owner_->includes(parent);
            for (const auto child : old)
            {
                if (std::find(replacement.begin(), replacement.end(), child) == replacement.end())
                {
                    auto [position, inserted] = prepared_dependents_.try_emplace(
                        child.value(), owner_->dependents(child).begin(), owner_->dependents(child).end());
                    auto& values = position->second;
                    values.erase(std::remove(values.begin(), values.end(), parent), values.end());
                }
            }
            for (const auto child : replacement)
            {
                if (std::find(old.begin(), old.end(), child) == old.end())
                {
                    auto [position, inserted] = prepared_dependents_.try_emplace(
                        child.value(), owner_->dependents(child).begin(), owner_->dependents(child).end());
                    auto& values = position->second;
                    if (std::find(values.begin(), values.end(), parent) == values.end())
                        values.push_back(parent);
                }
            }
        }
    }
    catch (const std::bad_alloc&)
    { return failure_ = {status_code::initialization_failed}; }
    catch (const std::length_error&)
    { return failure_ = {status_code::initialization_failed}; }
    prepared_ = true;
    return {};
}

void source_manager_update::publish_prepared() noexcept
{
    assert(prepared_ && !committed_ && owner_ != nullptr);
    owner_->physical_.resize(owner_->sources_.size() + added_sources_.size());
    owner_->includes_.resize(owner_->sources_.size() + added_sources_.size());
    owner_->dependents_.resize(owner_->sources_.size() + added_sources_.size());
    for (auto& source : added_sources_) owner_->sources_.push_back(std::move(source));
    owner_->by_path_.merge(added_by_path_);
    owner_->roots_.swap(roots_);
    for (auto& [value, includes] : include_delta_)
        owner_->includes_[value - 1].swap(includes);
    for (auto& [value, dependents] : prepared_dependents_)
        owner_->dependents_[value - 1].swap(dependents);
    for (auto& [value, delta] : physical_delta_)
    {
        auto& committed = owner_->physical_[value - 1];
        if (delta->observation_only)
        {
            if (committed) committed->state.observation = delta->state.observation;
        }
        else committed = std::move(delta->replacement);
    }
    owner_->next_source_id_ = next_source_id_;
    ++owner_->generation_;
    committed_ = true;
}

void source_manager_update::cancel() noexcept
{
    if (!committed_)
    {
        prepared_ = true;
        failure_ = {status_code::invalid_state};
    }
}
} // namespace cw::server
