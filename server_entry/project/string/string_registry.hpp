#pragma once

#include "../../status.hpp"
#include "../../string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cw::server {

class string_registry_update;
class graph_build_transaction;
class graph_build_transaction_test_access;
class graph_update;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

struct string_registry_storage_snapshot {
    const void* records_data = nullptr;
    std::size_t records_size = 0;
    std::size_t records_capacity = 0;
    std::size_t lookup_size = 0;
    std::size_t lookup_bucket_count = 0;
};

#endif

// Owns canonical project string identities. string_id is the direct one-based
// slot coordinate. Text-to-ID binding is construction-only; Graph and Runtime
// consume string_id and never perform textual lookup.
class string_registry final {
public:
    string_registry() = default;
    string_registry(const string_registry&) = delete;
    string_registry& operator=(const string_registry&) = delete;
    string_registry(string_registry&&) = delete;
    string_registry& operator=(string_registry&&) = delete;

    [[nodiscard]] status initialize() noexcept;
    [[nodiscard]] string_registry_update begin_update() noexcept;

    [[nodiscard]] std::optional<std::string_view> get(
        string_id id) const noexcept;

#if defined(CW_GRAPH_BUILD_TRANSACTION_TESTING)

    // Test observer only. Production code has no text-to-ID query after the
    // construction String Binding boundary.
    [[nodiscard]] string_id find_for_test(
        std::string_view value) const noexcept {
        return find_value(
            value,
            hash_value(value));
    }

#endif

    [[nodiscard]] std::size_t size() const noexcept {
        return records.size();
    }

    [[nodiscard]] std::size_t live_size() const noexcept {
        return live_count;
    }

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

    static constexpr std::uint32_t invalid_block =
        (std::numeric_limits<std::uint32_t>::max)();

    struct string_record {
        std::uint64_t hash = 0;
        std::uint64_t offset = 0;
        std::uint32_t block = invalid_block;
        std::uint32_t length = 0;

        [[nodiscard]] bool live() const noexcept {
            return block != invalid_block;
        }
    };

    struct byte_block {
        std::vector<char> bytes;
    };

    [[nodiscard]] static std::uint64_t hash_value(
        std::string_view value) noexcept;

    [[nodiscard]] string_id find_value(
        std::string_view value,
        std::uint64_t hash) const noexcept;

    std::vector<byte_block> blocks;
    std::vector<string_record> records;

    // Construction-only canonicalization index. Bucket N stores a raw string_id;
    // zero is empty. This table is never used by Graph or Runtime consumers.
    std::vector<std::uint32_t> index;

    std::size_t live_count = 0;
    std::uint64_t generation = 0;
};

// One prehashed spelling admitted at the construction String Binding boundary.
// hash is acceleration metadata only; equal bytes remain the identity test.
struct prehashed_string_binding {
    std::string_view value;
    std::uint64_t hash = 0;
};

// Builds one isolated String Registry candidate. bind() is the only textual
// identity boundary: it maps bytes to one canonical string_id. All later
// Builder/Graph work must use that ID directly.
class string_registry_update final {
public:
    ~string_registry_update() = default;

    string_registry_update(const string_registry_update&) = delete;
    string_registry_update& operator=(const string_registry_update&) = delete;

    string_registry_update(string_registry_update&& other) noexcept;
    string_registry_update& operator=(string_registry_update&&) = delete;

    [[nodiscard]] status reserve_bindings(
        std::size_t count,
        std::size_t bytes) noexcept;

    [[nodiscard]] status bind(
        std::string_view value,
        string_id& result) noexcept;

    // Admits one Source-local batch whose hashes were prepared by Parser
    // workers. Input order is authoritative for allocation of new string_id
    // values and therefore preserves the existing deterministic contract.
    [[nodiscard]] status bind_prehashed(
        std::span<const prehashed_string_binding> values,
        std::span<string_id> results) noexcept;

    [[nodiscard]] std::optional<std::string_view> get(
        string_id id) const noexcept;

    [[nodiscard]] std::size_t added_size() const noexcept {
        return added_records.size();
    }

    [[nodiscard]] status commit() noexcept;

private:
    friend class string_registry;
    friend class graph_build_transaction;
    friend class graph_update;

    explicit string_registry_update(
        string_registry& owner,
        std::uint64_t generation) noexcept;

    [[nodiscard]] status ensure_added_index(
        std::size_t required) noexcept;

    [[nodiscard]] status bind_prehashed_one(
        std::string_view value,
        std::uint64_t hash,
        string_id& result,
        bool capacity_ready) noexcept;

    [[nodiscard]] status prepare_publish() noexcept;

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

    string_registry* owner = nullptr;

    std::vector<string_registry::string_record> added_records;
    std::vector<char> added_bytes;
    // Update-local open-addressing buckets pack a 32-bit hash fingerprint with
    // the one-based local added-record coordinate. Most failed probes therefore
    // avoid touching added_records and added_bytes.
    std::vector<std::uint64_t> added_index;

    std::vector<string_registry::byte_block> rebuilt_blocks;
    std::vector<string_registry::string_record> rebuilt_records;
    std::vector<std::uint32_t> rebuilt_index;
    std::size_t rebuilt_live_count = 0;

    std::vector<std::uint32_t> prepared_index;
    bool index_rebuild_prepared = false;

    std::uint64_t base_generation = 0;
    status failure{};
    bool committed = false;
    bool prepared = false;
    bool rebuild_compaction_prepared = false;
};

} // namespace cw::server