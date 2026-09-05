#include "source_frontend_cache.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server {

const source_environment_storage* source_frontend_cache::interface(
    source_id source) const noexcept {

    if (!source ||
        source.value() > interfaces.size()) {
        return nullptr;
    }

    return interfaces[source.value() - 1].get();
}

source_frontend_cache_update source_frontend_cache::begin_update(
    bool full_reconstruction) noexcept {

    return source_frontend_cache_update{
        *this,
        full_reconstruction
    };
}

void source_frontend_cache::invalidate() noexcept {
    interfaces.clear();
    complete_state = false;
}

source_frontend_cache_update::source_frontend_cache_update(
    source_frontend_cache& owner,
    bool full_reconstruction) noexcept
    : owner(&owner),
      old_size(owner.interfaces.size()),
      full_reconstruction(full_reconstruction) {}

source_frontend_cache_update::~source_frontend_cache_update() {
    cancel();
}

source_frontend_cache_update::source_frontend_cache_update(
    source_frontend_cache_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      replacements(std::move(other.replacements)),
      old_size(other.old_size),
      full_reconstruction(other.full_reconstruction),
      prepared(other.prepared),
      published(other.published),
      failure(other.failure) {}

status source_frontend_cache_update::replace(
    source_id source,
    std::unique_ptr<source_environment_storage> interface) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (!source ||
        owner == nullptr ||
        prepared ||
        published) {
        return {status_code::invalid_state};
    }

    try {
        const auto duplicate = std::find_if(
            replacements.begin(),
            replacements.end(),
            [source](const replacement& item) noexcept {
                return item.source == source;
            });

        if (duplicate != replacements.end()) {
            failure = {status_code::invalid_state};
            return failure;
        }

        replacements.push_back({
            source,
            std::move(interface)
        });

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

status source_frontend_cache_update::prepare_publish(
    std::size_t required_source_count) noexcept {

    if (!failure.ok()) {
        return failure;
    }

    if (owner == nullptr ||
        prepared ||
        published) {
        return {status_code::invalid_state};
    }

    try {
        old_size = owner->interfaces.size();

        if (owner->interfaces.size() <
            required_source_count) {
            owner->interfaces.resize(
                required_source_count);
        }

        prepared = true;
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

void source_frontend_cache_update::publish_prepared() noexcept {
    if (!prepared ||
        published ||
        owner == nullptr) {
        return;
    }

    if (full_reconstruction) {
        for (auto& interface : owner->interfaces) {
            interface.reset();
        }
    }

    for (auto& item : replacements) {
        if (item.source &&
            item.source.value() <= owner->interfaces.size()) {
            owner->interfaces[item.source.value() - 1] =
                std::move(item.interface);
        }
    }

    owner->complete_state = true;
    published = true;
    prepared = false;
    owner = nullptr;
}

void source_frontend_cache_update::cancel() noexcept {
    if (owner == nullptr ||
        published) {
        return;
    }

    if (prepared &&
        owner->interfaces.size() > old_size) {
        owner->interfaces.resize(old_size);
    }

    owner = nullptr;
    prepared = false;
}

} // namespace cw::server
