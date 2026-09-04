#pragma once

#include "../../status.hpp"
#include "../../string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cw::server {

class string_registry_update;
class graph_build_transaction;
class graph_build_transaction_test_access;
class graph_update;

// Owns the canonical interned strings used by Graph construction and access.
// String bytes remain at stable addresses so registry lookup keys and returned
// string_views stay valid until the registry is reinitialized or replaced.
// string_id values are one-based registry slot identities assigned on publish.
// G0 reclamation may tombstone an unused slot but never renumbers or reuses it.
class string_registry final {
public:
    string_registry() = default;
    string_registry(const string_registry&) = delete;
    string_registry& operator=(const string_registry&) = delete;
    string_registry(string_registry&&) = delete;
    string_registry& operator=(string_registry&&) = delete;

    [[nodiscard]] status initialize() noexcept;
    [[nodiscard]] string_registry_update begin_update() noexcept;
    [[nodiscard]] string_id find(std::string_view value) const noexcept;
    [[nodiscard]] std::optional<std::string_view> get(string_id id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return records.size(); }
    [[nodiscard]] std::size_t live_size() const noexcept { return lookup_index.size(); }
    [[nodiscard]] status rebuild_lookup_index() noexcept;
    [[nodiscard]] status export_slots(
        std::vector<std::optional<std::string>>& output) const noexcept;
    [[nodiscard]] status import_slots(
        std::span<const std::optional<std::string>> values) noexcept;
    void swap_compiled(string_registry& other) noexcept;

private:
    friend class string_registry_update;
    friend class graph_build_transaction;
    friend class graph_update;
    friend class graph_build_transaction_test_access;

    struct string_record {
        std::string bytes;
    };

    struct string_view_hash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    using lookup_type = std::unordered_map<
        std::string_view,
        string_id,
        string_view_hash,
        std::equal_to<>>;

    // storage owns bytes and therefore the lifetime of lookup string_views.
    // records is the direct string_id-to-Entry table; slot N is records[N - 1].
    // A null record is a G0-reclaimed tombstone. Numeric slots are never reused.
    std::list<string_record> storage;
    std::vector<const string_record*> records;
    lookup_type lookup_index;
    std::uint64_t generation = 0;
};

// Builds an isolated append-only update against one string_registry generation.
// New strings and indexes remain update-local until publication; any owner
// generation change invalidates the update so stale candidates cannot commit.
class string_registry_update final {
public:
    ~string_registry_update() = default;
    string_registry_update(const string_registry_update&) = delete;
    string_registry_update& operator=(const string_registry_update&) = delete;
    string_registry_update(string_registry_update&& other) noexcept;
    string_registry_update& operator=(string_registry_update&&) = delete;

    [[nodiscard]] string_id find(std::string_view value) const noexcept;
    [[nodiscard]] status intern(std::string_view value, string_id& result) noexcept;
    [[nodiscard]] std::optional<std::string_view> get(string_id id) const noexcept;
    [[nodiscard]] std::size_t added_size() const noexcept {
        return added_records.size();
    }

    [[nodiscard]] status commit() noexcept;

private:
    friend class string_registry;
    friend class graph_build_transaction;
    friend class graph_update;

    string_registry_update(
        string_registry& owner,
        std::uint64_t generation) noexcept;

    string_registry* owner = nullptr;
    std::list<string_registry::string_record> added_storage;
    std::vector<const string_registry::string_record*> added_records;
    string_registry::lookup_type added_lookup;

    // G0-only physical string reclamation. The rebuilt records vector preserves
    // every numeric string_id slot while unretained slots become nullptr.
    std::list<string_registry::string_record> rebuilt_storage;
    std::vector<const string_registry::string_record*> rebuilt_records;
    string_registry::lookup_type rebuilt_lookup;
    std::uint64_t base_generation = 0;
    status failure{};
    bool committed = false;
    bool prepared = false;
    bool rebuild_compaction_prepared = false;

    // Reserves all destination hash-table capacity before publication so the
    // subsequent splice/merge publish step cannot fail through allocation.
    [[nodiscard]] status prepare_publish() noexcept;

    // Builds a compact physical registry for G0 while preserving all string_id
    // slot numbers. retained[id] must be nonzero for every semantic/historical
    // string that must survive the rebuild.
    [[nodiscard]] status prepare_rebuild_compaction(
        std::span<const std::uint8_t> retained) noexcept;

    void publish_prepared() noexcept;
    void cancel() noexcept;

    [[nodiscard]] std::optional<std::string_view>
        get_for_validation(string_id id) const noexcept;

    [[nodiscard]] std::size_t candidate_size_for_validation() const noexcept {
        return owner == nullptr
                   ? 0
                   : owner->records.size() + added_records.size();
    }
};

} // namespace cw::server
