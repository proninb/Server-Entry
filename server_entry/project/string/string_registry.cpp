#include "string_registry.hpp"

#include <cassert>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server
{

status string_registry::initialize() noexcept
{
    lookup_index_.clear();
    records_.clear();
    storage_.clear();
    ++generation_;
    return {};
}

string_registry_update string_registry::begin_update() noexcept
{
    return string_registry_update{*this, generation_};
}

string_id string_registry::find(std::string_view value) const noexcept
{
    const auto found = lookup_index_.find(value);
    return found == lookup_index_.end() ? string_id{} : found->second;
}

std::optional<std::string_view> string_registry::get(string_id id) const noexcept
{
    if (!id || id.value() > records_.size()) return std::nullopt;
    const auto found = records_.find(id.value());
    if (found == records_.end()) return std::nullopt;
    return std::string_view{found->second->bytes};
}

status string_registry::rebuild_lookup_index() noexcept
{
    try
    {
        lookup_type rebuilt;
        rebuilt.reserve(records_.size());
        for (std::size_t index = 0; index != records_.size(); ++index)
        {
            const auto value = static_cast<std::uint32_t>(index + 1);
            const auto record_position = records_.find(value);
            if (record_position == records_.end()) return {status_code::invalid_state};
            const auto* record = record_position->second;
            const auto [position, inserted] =
                rebuilt.emplace(std::string_view{record->bytes}, string_id{value});
            (void)position;
            if (!inserted) return {status_code::invalid_state};
        }
        lookup_index_.swap(rebuilt);
        return {};
    }
    catch (const std::bad_alloc&) { return {status_code::initialization_failed}; }
    catch (const std::length_error&) { return {status_code::initialization_failed}; }
}

status string_registry::export_dense(std::vector<std::string>& output) const noexcept
{
    try{output.clear();output.reserve(records_.size());for(std::uint32_t id=1;id<=records_.size();++id){const auto value=get(string_id{id});if(!value)return{status_code::invalid_state};output.emplace_back(*value);}return{};}
    catch(...){output.clear();return{status_code::initialization_failed};}
}

status string_registry::import_dense(std::span<const std::string> values) noexcept
{
    string_registry candidate;if(!candidate.initialize().ok())return{status_code::initialization_failed};auto update=candidate.begin_update();std::uint32_t expected=1;
    for(const auto& value:values){string_id id;const auto result=update.intern(value,id);if(!result.ok())return result;if(id.value()!=expected++)return{status_code::artifact_corrupt};}
    const auto result=update.commit();if(!result.ok())return result;storage_.swap(candidate.storage_);records_.swap(candidate.records_);lookup_index_.swap(candidate.lookup_index_);++generation_;return{};
}

void string_registry::swap_compiled(string_registry& other) noexcept
{ storage_.swap(other.storage_);records_.swap(other.records_);lookup_index_.swap(other.lookup_index_);std::swap(generation_,other.generation_); }

string_registry_update::string_registry_update(string_registry& owner,
                                               std::uint64_t generation) noexcept
    : owner_(&owner), base_generation_(generation)
{
}

string_registry_update::string_registry_update(string_registry_update&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      added_storage_(std::move(other.added_storage_)),
      added_records_(std::move(other.added_records_)),
      added_by_id_(std::move(other.added_by_id_)),
      added_lookup_(std::move(other.added_lookup_)),
      base_generation_(other.base_generation_), failure_(other.failure_),
      committed_(other.committed_), prepared_(other.prepared_)
{
}

string_id string_registry_update::find(std::string_view value) const noexcept
{
    if (owner_ == nullptr || committed_ || prepared_ ||
        owner_->generation_ != base_generation_) return {};
    const auto added = added_lookup_.find(value);
    if (added != added_lookup_.end()) return added->second;
    return owner_->find(value);
}

status string_registry_update::intern(std::string_view value, string_id& result) noexcept
{
    result = {};
    if (!failure_.ok()) return failure_;
    if (owner_ == nullptr || committed_ || prepared_ ||
        owner_->generation_ != base_generation_)
        return {status_code::invalid_state};
    if (const auto existing = find(value); existing)
    {
        result = existing;
        return {};
    }
    const auto next = owner_->records_.size() + added_records_.size() + 1;
    if (next > std::numeric_limits<std::uint32_t>::max())
        return failure_ = {status_code::initialization_failed};

    try
    {
        added_storage_.push_back({std::string{value}});
        const auto* record = &added_storage_.back();
        const auto id = string_id{static_cast<std::uint32_t>(next)};
        try { added_records_.push_back(record); }
        catch (...) { added_storage_.pop_back(); throw; }
        try { added_by_id_.emplace(id.value(), record); }
        catch (...)
        {
            added_records_.pop_back();
            added_storage_.pop_back();
            throw;
        }
        try
        {
            const auto [position, inserted] =
                added_lookup_.emplace(std::string_view{record->bytes}, id);
            if (!inserted)
            {
                added_records_.pop_back();
                added_by_id_.erase(id.value());
                added_storage_.pop_back();
                result = position->second;
                return {};
            }
        }
        catch (...)
        {
            added_records_.pop_back();
            added_by_id_.erase(id.value());
            added_storage_.pop_back();
            throw;
        }
        result = id;
        return {};
    }
    catch (const std::bad_alloc&)
    { return failure_ = {status_code::initialization_failed}; }
    catch (const std::length_error&)
    { return failure_ = {status_code::initialization_failed}; }
}

std::optional<std::string_view> string_registry_update::get(string_id id) const noexcept
{
    if (!id || owner_ == nullptr || committed_ || prepared_ ||
        owner_->generation_ != base_generation_) return std::nullopt;
    if (id.value() <= owner_->records_.size()) return owner_->get(id);
    const auto offset = id.value() - owner_->records_.size() - 1;
    if (offset >= added_records_.size()) return std::nullopt;
    return std::string_view{added_records_[offset]->bytes};
}

std::optional<std::string_view>
string_registry_update::get_for_validation(string_id id) const noexcept
{
    if (!id || owner_ == nullptr || committed_ || owner_->generation_ != base_generation_)
        return std::nullopt;
    if (id.value() <= owner_->records_.size()) return owner_->get(id);
    const auto offset = id.value() - owner_->records_.size() - 1;
    if (offset >= added_records_.size()) return std::nullopt;
    return std::string_view{added_records_[offset]->bytes};
}

status string_registry_update::commit() noexcept
{
    const auto result = prepare_publish();
    if (!result.ok()) return result;
    publish_prepared();
    return {};
}

status string_registry_update::prepare_publish() noexcept
{
    if (!failure_.ok()) return failure_;
    if (owner_ == nullptr || committed_ || prepared_)
        return {status_code::invalid_state};
    if (base_generation_ != owner_->generation_)
        return failure_ = {status_code::invalid_state};
    try
    {
        owner_->records_.reserve(owner_->records_.size() + added_by_id_.size());
        owner_->lookup_index_.reserve(owner_->lookup_index_.size() + added_lookup_.size());
    }
    catch (const std::bad_alloc&)
    {
        return failure_ = {status_code::initialization_failed};
    }
    catch (const std::length_error&)
    {
        return failure_ = {status_code::initialization_failed};
    }
    prepared_ = true;
    return {};
}

void string_registry_update::publish_prepared() noexcept
{
    assert(prepared_ && !committed_ && owner_ != nullptr);
    owner_->storage_.splice(owner_->storage_.end(), added_storage_);
    owner_->records_.merge(added_by_id_);
    owner_->lookup_index_.merge(added_lookup_);
    assert(added_by_id_.empty());
    assert(added_lookup_.empty());
    ++owner_->generation_;
    committed_ = true;
}

void string_registry_update::cancel() noexcept
{
    if (!committed_)
    {
        prepared_ = true;
        failure_ = {status_code::invalid_state};
    }
}

} // namespace cw::server
