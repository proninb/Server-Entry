#include "source_manager.hpp"

#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server
{
source_manager_update::source_manager_update(source_manager_update&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      added_sources_(std::move(other.added_sources_)),
      roots_(std::move(other.roots_)),
      added_by_path_(std::move(other.added_by_path_)),
      next_source_id_(other.next_source_id_),
      base_generation_(other.base_generation_),
      failure_(other.failure_),
      committed_(other.committed_)
{
}

status source_manager_update::add(const std::filesystem::path& path,
                                  project_item_role role) noexcept
{
    if (!failure_.ok()) return failure_;
    if (committed_ || owner_ == nullptr) return {status_code::invalid_state};
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
        roots_.push_back({id, role});
        return {};
    }
    catch (const std::bad_alloc&) { failure_ = {status_code::initialization_failed}; }
    catch (const std::length_error&) { failure_ = {status_code::initialization_failed}; }
    catch (const std::filesystem::filesystem_error&)
    { failure_ = {status_code::configuration_failed}; }
    return failure_;
}

status source_manager_update::commit() noexcept
{
    if (!failure_.ok()) return failure_;
    if (committed_ || owner_ == nullptr) return {status_code::invalid_state};
    const auto result = owner_->commit(*this);
    if (!result.ok()) { failure_ = result; return result; }
    committed_ = true;
    return {};
}

status source_manager::initialize() noexcept { return {}; }
source_manager_update source_manager::begin_update() noexcept
{
    return source_manager_update{*this, next_source_id_, generation_};
}

const source_record* source_manager::find(source_id id) const noexcept
{
    if (!id || id.value() > sources_.size()) return nullptr;
    const auto& candidate = sources_[id.value() - 1];
    return candidate.id == id ? &candidate : nullptr;
}

std::span<const source_root> source_manager::roots() const noexcept { return roots_; }
std::span<const source_record> source_manager::sources() const noexcept { return sources_; }

status source_manager::commit(source_manager_update& update) noexcept
{
    if (update.base_generation_ != generation_) return {status_code::invalid_state};
    try
    {
        sources_.reserve(sources_.size() + update.added_sources_.size());
        by_path_.reserve(by_path_.size() + update.added_by_path_.size());
    }
    catch (const std::bad_alloc&) { return {status_code::initialization_failed}; }
    catch (const std::length_error&) { return {status_code::initialization_failed}; }

    for (auto& source : update.added_sources_) sources_.push_back(std::move(source));
    by_path_.merge(update.added_by_path_);
    roots_.swap(update.roots_);
    next_source_id_ = update.next_source_id_;
    ++generation_;
    return {};
}
} // namespace cw::server
