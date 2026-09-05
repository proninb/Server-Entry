#pragma once

#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace cw::server {

class source_frontend_cache_update;

// Retains Parser-visible Source interfaces between committed frontend generations.
// This is COLD acceleration state only; canonical identity and SourceContribution
// remain owned by their existing subsystems.
class source_frontend_cache final {
public:
    source_frontend_cache() = default;

    [[nodiscard]] bool complete() const noexcept {
        return complete_state;
    }

    [[nodiscard]] const source_environment_storage* interface(
        source_id source) const noexcept;

    [[nodiscard]] source_frontend_cache_update begin_update(
        bool full_reconstruction = false) noexcept;

    void invalidate() noexcept;

private:
    friend class source_frontend_cache_update;

    std::vector<std::unique_ptr<source_environment_storage>> interfaces;
    bool complete_state = false;
};

// Builds one sparse candidate update of persistent frontend interfaces.
// Structural growth is prepared before Graph commit; publication then consists
// only of noexcept unique_ptr moves.
class source_frontend_cache_update final {
public:
    source_frontend_cache_update() noexcept = default;
    ~source_frontend_cache_update();

    source_frontend_cache_update(
        const source_frontend_cache_update&) = delete;
    source_frontend_cache_update& operator=(
        const source_frontend_cache_update&) = delete;

    source_frontend_cache_update(
        source_frontend_cache_update&& other) noexcept;
    source_frontend_cache_update& operator=(
        source_frontend_cache_update&&) = delete;

    [[nodiscard]] status replace(
        source_id source,
        std::unique_ptr<source_environment_storage> interface) noexcept;

    [[nodiscard]] status prepare_publish(
        std::size_t required_source_count) noexcept;

    void publish_prepared() noexcept;
    void cancel() noexcept;

private:
    friend class source_frontend_cache;

    struct replacement {
        source_id source{};
        std::unique_ptr<source_environment_storage> interface;
    };

    source_frontend_cache_update(
        source_frontend_cache& owner,
        bool full_reconstruction) noexcept;

    source_frontend_cache* owner = nullptr;
    std::vector<replacement> replacements;
    std::size_t old_size = 0;
    bool full_reconstruction = false;
    bool prepared = false;
    bool published = false;
    status failure{};
};

} // namespace cw::server
