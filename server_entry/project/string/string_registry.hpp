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

namespace cw::server
{

class string_registry_update;
class graph_build_transaction;
class graph_build_transaction_test_access;
class graph_update;

class string_registry final
{
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
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] status rebuild_lookup_index() noexcept;
    [[nodiscard]] status export_dense(std::vector<std::string>& output) const noexcept;
    [[nodiscard]] status import_dense(std::span<const std::string> values) noexcept;
    void swap_compiled(string_registry& other) noexcept;

private:
    friend class string_registry_update;
    friend class graph_build_transaction;
    friend class graph_update;
    friend class graph_build_transaction_test_access;
    struct string_record { std::string bytes; };
    struct string_view_hash
    {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
        { return std::hash<std::string_view>{}(value); }
    };
    using lookup_type = std::unordered_map<
        std::string_view, string_id, string_view_hash, std::equal_to<>>;
    using id_lookup_type = std::unordered_map<std::uint32_t, const string_record*>;

    // storage_ owns bytes; records_ is dense string_id -> stable record.
    std::list<string_record> storage_;
    id_lookup_type records_;
    lookup_type lookup_index_;
    std::uint64_t generation_ = 0;
};

class string_registry_update final
{
public:
    ~string_registry_update() = default;
    string_registry_update(const string_registry_update&) = delete;
    string_registry_update& operator=(const string_registry_update&) = delete;
    string_registry_update(string_registry_update&& other) noexcept;
    string_registry_update& operator=(string_registry_update&&) = delete;

    [[nodiscard]] string_id find(std::string_view value) const noexcept;
    [[nodiscard]] status intern(std::string_view value, string_id& result) noexcept;
    [[nodiscard]] std::optional<std::string_view> get(string_id id) const noexcept;
    [[nodiscard]] std::size_t added_size() const noexcept { return added_records_.size(); }
    [[nodiscard]] status commit() noexcept;

private:
    friend class string_registry;
    friend class graph_build_transaction;
    friend class graph_update;
    string_registry_update(string_registry& owner, std::uint64_t generation) noexcept;

    string_registry* owner_ = nullptr;
    std::list<string_registry::string_record> added_storage_;
    std::vector<const string_registry::string_record*> added_records_;
    string_registry::id_lookup_type added_by_id_;
    string_registry::lookup_type added_lookup_;
    std::uint64_t base_generation_ = 0;
    status failure_{};
    bool committed_ = false;
    bool prepared_ = false;

    [[nodiscard]] status prepare_publish() noexcept;
    void publish_prepared() noexcept;
    void cancel() noexcept;
    [[nodiscard]] std::optional<std::string_view>
        get_for_validation(string_id id) const noexcept;
    [[nodiscard]] std::size_t candidate_size_for_validation() const noexcept
    { return owner_ == nullptr ? 0 : owner_->records_.size() + added_records_.size(); }
};

} // namespace cw::server
