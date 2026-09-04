#include "compiled_persistence.hpp"

#include "compiled_state.hpp"
#include "../string/string_registry.hpp"
#include "../../core/hash/sha256.hpp"
#include "../../metrics/metrics_store.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace cw::server {
namespace {

using persistence_clock = std::chrono::steady_clock;

// Frozen compiled-artifact container contract.
constexpr std::size_t header_size = 128;
constexpr std::size_t directory_offset = 128;
constexpr std::size_t section_entry_size = 32;
constexpr std::size_t section_count = 8;
constexpr std::size_t data_offset =
    directory_offset + section_entry_size * section_count;

constexpr std::uint16_t format_major = 1;
constexpr std::uint16_t format_minor = 6;
constexpr std::uint32_t endian_marker = 0x01020304;
constexpr std::size_t payload_hash_offset = 72;
constexpr std::size_t header_crc_offset = 104;

constexpr std::array<char, 8> magic{
    'C', 'W', 'C', 'O', 'M', 'P', 'I', '1'
};

enum class section_index : std::size_t {
    strings = 0,
    string_bytes,
    identities,
    entities,
    types,
    enum_values,
    members,
    canonical_types
};

constexpr std::size_t to_index(section_index section) noexcept {
    return static_cast<std::size_t>(section);
}

constexpr std::array<std::uint32_t, section_count> section_record_sizes{
    8, 1, 4, 16, 16, 16, 8, 16
};

struct section_descriptor {
    std::uint32_t kind = 0;
    std::uint32_t record_size = 0;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
    std::uint32_t count = 0;
};

void write16(std::byte* data, std::uint16_t value) noexcept {
    data[0] = std::byte(value & 0xff);
    data[1] = std::byte((value >> 8) & 0xff);
}

void write32(std::byte* data, std::uint32_t value) noexcept {
    for (unsigned index = 0; index != 4; ++index) {
        data[index] =
            std::byte((value >> (index * 8)) & 0xff);
    }
}

void write64(std::byte* data, std::uint64_t value) noexcept {
    for (unsigned index = 0; index != 8; ++index) {
        data[index] =
            std::byte((value >> (index * 8)) & 0xff);
    }
}

std::uint16_t read16(const std::byte* data) noexcept {
    const auto low =
        static_cast<std::uint16_t>(
            std::to_integer<unsigned char>(data[0]));

    const auto high =
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                std::to_integer<unsigned char>(data[1])) << 8);

    return static_cast<std::uint16_t>(low | high);
}

std::uint32_t read32(const std::byte* data) noexcept {
    std::uint32_t value = 0;

    for (unsigned index = 0; index != 4; ++index) {
        value |=
            std::uint32_t(std::to_integer<unsigned char>(data[index]))
            << (index * 8);
    }

    return value;
}

std::uint64_t read64(const std::byte* data) noexcept {
    std::uint64_t value = 0;

    for (unsigned index = 0; index != 8; ++index) {
        value |=
            std::uint64_t(std::to_integer<unsigned char>(data[index]))
            << (index * 8);
    }

    return value;
}

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = ~0u;

    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);

        for (unsigned bit = 0; bit != 8; ++bit) {
            crc =
                (crc >> 1) ^
                ((crc & 1u) ? 0x82f63b78u : 0u);
        }
    }

    return ~crc;
}

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {

    if (left >
        (std::numeric_limits<std::uint64_t>::max)() - right) {
        return false;
    }

    output = left + right;
    return true;
}

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {

    if (left != 0 &&
        right >
            (std::numeric_limits<std::uint64_t>::max)() / left) {
        return false;
    }

    output = left * right;
    return true;
}

bool align8(
    std::uint64_t value,
    std::uint64_t& output) noexcept {

    std::uint64_t adjusted = 0;

    if (!checked_add(value, 7, adjusted)) {
        return false;
    }

    output = adjusted & ~std::uint64_t{7};
    return true;
}

void record_duration(
    metrics_store* metrics,
    metric_id id,
    persistence_clock::time_point begin) noexcept {

    if (!metrics) {
        return;
    }

    metrics->record_duration(
        id,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            persistence_clock::now() - begin));
}

status build_sections(
    const compiled_project_state& compiled,
    std::array<section_descriptor, section_count>& sections,
    std::uint64_t& total_size) noexcept {

    std::uint64_t string_bytes = 0;

    for (const auto& value : compiled.strings) {
        if (value &&
            !checked_add(
                string_bytes,
                value->size(),
                string_bytes)) {
            return {status_code::initialization_failed};
        }
    }

    const auto fits_u32 = [](std::uint64_t value) noexcept {
        return
            value <=
            (std::numeric_limits<std::uint32_t>::max)();
    };

    if (!fits_u32(compiled.strings.size()) ||
        !fits_u32(string_bytes) ||
        !fits_u32(compiled.graph.identities.size()) ||
        !fits_u32(compiled.graph.entities.size()) ||
        !fits_u32(compiled.graph.types.size()) ||
        !fits_u32(compiled.graph.enum_values.size()) ||
        !fits_u32(compiled.graph.members.size()) ||
        !fits_u32(compiled.graph.canonical_types.size())) {
        return {status_code::initialization_failed};
    }

    const std::array<std::uint32_t, section_count> counts{
        static_cast<std::uint32_t>(compiled.strings.size()),
        static_cast<std::uint32_t>(string_bytes),
        static_cast<std::uint32_t>(compiled.graph.identities.size()),
        static_cast<std::uint32_t>(compiled.graph.entities.size()),
        static_cast<std::uint32_t>(compiled.graph.types.size()),
        static_cast<std::uint32_t>(compiled.graph.enum_values.size()),
        static_cast<std::uint32_t>(compiled.graph.members.size()),
        static_cast<std::uint32_t>(compiled.graph.canonical_types.size())
    };

    std::uint64_t next = data_offset;

    for (std::size_t index = 0; index != section_count; ++index) {
        std::uint64_t offset = 0;
        std::uint64_t bytes = 0;

        if (!align8(next, offset) ||
            !checked_multiply(
                counts[index],
                section_record_sizes[index],
                bytes) ||
            !checked_add(offset, bytes, next)) {
            return {status_code::initialization_failed};
        }

        sections[index] = {
            static_cast<std::uint32_t>(index + 1),
            section_record_sizes[index],
            offset,
            bytes,
            counts[index]
        };
    }

    total_size = next;
    return {};
}

status validate_directory(
    const std::vector<std::byte>& bytes,
    std::array<section_descriptor, section_count>& sections) noexcept {

    std::uint64_t previous_end = data_offset;

    for (std::size_t index = 0; index != section_count; ++index) {
        const auto* entry =
            bytes.data() +
            directory_offset +
            index * section_entry_size;

        const auto kind = read32(entry);
        const auto flags = read32(entry + 4);
        const auto offset = read64(entry + 8);
        const auto byte_count = read64(entry + 16);
        const auto count = read32(entry + 24);
        const auto record_size = read32(entry + 28);

        std::uint64_t exact_bytes = 0;

        if (kind != index + 1 ||
            flags != 0 ||
            record_size != section_record_sizes[index] ||
            !checked_multiply(count, record_size, exact_bytes) ||
            byte_count != exact_bytes ||
            (offset & 7u) != 0 ||
            offset < previous_end ||
            offset > bytes.size() ||
            byte_count > bytes.size() - offset) {
            return {status_code::artifact_corrupt};
        }

        for (std::uint64_t position = previous_end;
             position < offset;
             ++position) {
            if (bytes[static_cast<std::size_t>(position)] !=
                std::byte{}) {
                return {status_code::artifact_corrupt};
            }
        }

        sections[index] = {
            kind,
            record_size,
            offset,
            byte_count,
            count
        };

        previous_end = offset + byte_count;
    }

    return
        previous_end == bytes.size()
            ? status{}
            : status{status_code::artifact_corrupt};
}

status validate_header(const std::vector<std::byte>& bytes) noexcept {
    if (bytes.size() < data_offset ||
        std::memcmp(bytes.data(), magic.data(), magic.size()) != 0 ||
        read16(bytes.data() + 8) != format_major ||
        read16(bytes.data() + 10) != format_minor ||
        read32(bytes.data() + 12) != header_size ||
        read32(bytes.data() + 16) != endian_marker ||
        read64(bytes.data() + 24) != bytes.size() ||
        read32(bytes.data() + 32) != directory_offset ||
        read32(bytes.data() + 36) != section_count) {
        return {status_code::artifact_corrupt};
    }

    auto header =
        std::vector<std::byte>(
            bytes.begin(),
            bytes.begin() + header_size);

    const auto stored_crc =
        read32(header.data() + header_crc_offset);

    write32(header.data() + header_crc_offset, 0);

    if (crc32c(header) != stored_crc) {
        return {status_code::artifact_corrupt};
    }

    const auto digest = core::sha256({
        reinterpret_cast<const char*>(
            bytes.data() + header_size),
        bytes.size() - header_size
    });

    if (std::memcmp(
            digest.data(),
            bytes.data() + payload_hash_offset,
            digest.size()) != 0) {
        return {status_code::artifact_corrupt};
    }

    return {};
}

status decode_strings(
    const std::vector<std::byte>& bytes,
    const std::array<section_descriptor, section_count>& sections,
    compiled_project_state& compiled) {

    const auto& records =
        sections[to_index(section_index::strings)];

    const auto& storage =
        sections[to_index(section_index::string_bytes)];

    compiled.strings.reserve(records.count);

    for (std::uint32_t index = 0;
         index != records.count;
         ++index) {
        const auto* record =
            bytes.data() +
            records.offset +
            std::uint64_t(index) * records.record_size;

        const auto offset = read32(record);
        const auto length = read32(record + 4);

        if (offset ==
            (std::numeric_limits<std::uint32_t>::max)()) {
            if (length != 0) {
                return {status_code::artifact_corrupt};
            }

            compiled.strings.emplace_back(std::nullopt);
            continue;
        }

        if (offset > storage.bytes ||
            length > storage.bytes - offset) {
            return {status_code::artifact_corrupt};
        }

        compiled.strings.emplace_back(
            std::string{
                reinterpret_cast<const char*>(
                    bytes.data() + storage.offset + offset),
                length});
    }

    return {};
}

status decode_graph_sections(
    const std::vector<std::byte>& bytes,
    const std::array<section_descriptor, section_count>& sections,
    compiled_project_state& compiled) {

    const auto& identities = sections[to_index(section_index::identities)];
    compiled.graph.identities.resize(identities.count);

    for (std::uint32_t index = 0; index != identities.count; ++index) {
        compiled.graph.identities[index] =
            read32(bytes.data() + identities.offset +
                   std::uint64_t(index) * identities.record_size);
    }

    const auto& entities = sections[to_index(section_index::entities)];
    compiled.graph.entities.resize(entities.count);

    for (std::uint32_t index = 0; index != entities.count; ++index) {
        const auto* record = bytes.data() + entities.offset +
            std::uint64_t(index) * entities.record_size;

        const auto kind = std::to_integer<std::uint8_t>(record[0]);

        if (kind > std::uint8_t(entity_kind::enum_type) ||
            std::to_integer<std::uint8_t>(record[1]) != 0 ||
            read16(record + 2) != 0 ||
            read32(record + 12) != 0) {
            return {status_code::artifact_corrupt};
        }

        auto& entity = compiled.graph.entities[index];
        entity.kind = static_cast<entity_kind>(kind);
        entity.name = read32(record + 4);
        entity.type = read32(record + 8);

        if (!entity.live() && entity.type != 0) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto& types = sections[to_index(section_index::types)];
    const auto& enum_values = sections[to_index(section_index::enum_values)];
    const auto& members = sections[to_index(section_index::members)];

    compiled.graph.enum_values.resize(enum_values.count);
    for (std::uint32_t index = 0; index != enum_values.count; ++index) {
        const auto* record = bytes.data() + enum_values.offset +
            std::uint64_t(index) * enum_values.record_size;

        if (read32(record + 4) != 0) {
            return {status_code::artifact_corrupt};
        }

        auto& value = compiled.graph.enum_values[index];
        value.name = read32(record);
        value.bits = read64(record + 8);

        if (!value.name) {
            return {status_code::artifact_corrupt};
        }
    }

    compiled.graph.members.resize(members.count);
    for (std::uint32_t index = 0; index != members.count; ++index) {
        const auto* record = bytes.data() + members.offset +
            std::uint64_t(index) * members.record_size;

        auto& member = compiled.graph.members[index];
        member.name = read32(record);
        member.type_ref = read32(record + 4);

        if (!member.name || !member.type_ref) {
            return {status_code::artifact_corrupt};
        }
    }

    compiled.graph.types.resize(types.count);
    for (std::uint32_t index = 0; index != types.count; ++index) {
        const auto* record = bytes.data() + types.offset +
            std::uint64_t(index) * types.record_size;

        const auto live = std::to_integer<std::uint8_t>(record[0]);
        const auto kind = std::to_integer<std::uint8_t>(record[1]);
        const auto scoped = std::to_integer<std::uint8_t>(record[2]);
        const auto fixed_underlying = std::to_integer<std::uint8_t>(record[3]);
        const auto underlying = std::to_integer<std::uint8_t>(record[4]);
        if (live > 1 ||
            kind > std::uint8_t(user_type_kind::enumeration) ||
            scoped > 1 ||
            fixed_underlying > 1 ||
            underlying > std::uint8_t(builtin_type::long_double_floating) ||
            std::to_integer<std::uint8_t>(record[5]) != 0 ||
            read16(record + 6) != 0) {
            return {status_code::artifact_corrupt};
        }

        auto& type = compiled.graph.types[index];
        type.live = live != 0;

        if (!type.live) {
            if (scoped != 0 || fixed_underlying != 0 || underlying != 0 ||
                read32(record + 8) != 0 || read32(record + 12) != 0) {
                return {status_code::artifact_corrupt};
            }
            continue;
        }

        type.record.kind = static_cast<user_type_kind>(kind);

        if (type.record.kind == user_type_kind::enumeration) {
            type.record.enumeration.scoped = scoped != 0;
            type.record.enumeration.fixed_underlying = fixed_underlying != 0;
            type.record.enumeration.underlying = static_cast<builtin_type>(underlying);
        }
        else if (scoped != 0 || fixed_underlying != 0 || underlying != 0) {
            return {status_code::artifact_corrupt};
        }

        type.record.definition = {read32(record + 8), read32(record + 12)};

        if (!type.record.definition && type.record.definition.count != 0) {
            return {status_code::artifact_corrupt};
        }

        if (type.record.definition) {
            const auto begin = static_cast<std::uint64_t>(type.record.definition.begin - 1);
            const auto count = static_cast<std::uint64_t>(type.record.definition.count);
            const auto arena_count = type.record.kind == user_type_kind::aggregate
                ? static_cast<std::uint64_t>(members.count)
                : static_cast<std::uint64_t>(enum_values.count);

            if (begin > arena_count || count > arena_count - begin) {
                return {status_code::artifact_corrupt};
            }
        }
    }

    const auto& canonical_types = sections[to_index(section_index::canonical_types)];
    compiled.graph.canonical_types.resize(canonical_types.count);

    for (std::uint32_t index = 0; index != canonical_types.count; ++index) {
        const auto* record = bytes.data() + canonical_types.offset +
            std::uint64_t(index) * canonical_types.record_size;

        if (read16(record + 2) != 0) {
            return {status_code::artifact_corrupt};
        }

        auto& type = compiled.graph.canonical_types[index];
        type.kind = std::to_integer<std::uint8_t>(record[0]);
        type.subtype = std::to_integer<std::uint8_t>(record[1]);
        type.argument = read32(record + 4);
        type.payload = read64(record + 8);
    }

    return {};
}

status validate_cross_references(
    const compiled_project_state& compiled) noexcept {

    const auto string_count = compiled.strings.size();

    const auto string_is_live =
        [&](std::uint32_t id) noexcept {
            return id != 0 &&
                id <= string_count &&
                compiled.strings[id - 1].has_value();
        };

    if (compiled.graph.identities.size() > string_count + 1) {
        return {status_code::artifact_corrupt};
    }

    for (std::size_t name = 1;
         name < compiled.graph.identities.size();
         ++name) {
        if (compiled.graph.identities[name] != 0 &&
            !string_is_live(static_cast<std::uint32_t>(name))) {
            return {status_code::artifact_corrupt};
        }
    }

    for (const auto& entity : compiled.graph.entities) {
        if (entity.live() && !string_is_live(entity.name)) {
            return {status_code::artifact_corrupt};
        }
    }

    for (const auto& value : compiled.graph.enum_values) {
        if (!string_is_live(value.name)) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto canonical_type_count =
        compiled.graph.canonical_types.size();

    for (const auto& member : compiled.graph.members) {
        if (!string_is_live(member.name) ||
            !member.type_ref ||
            member.type_ref >= canonical_type_count) {
            return {status_code::artifact_corrupt};
        }
    }

    return {};
}

status decode_artifact(
    const std::vector<std::byte>& bytes,
    compiled_project_state* output) noexcept {

    try {
        auto result = validate_header(bytes);

        if (!result.ok()) {
            return result;
        }

        std::array<section_descriptor, section_count> sections{};

        result = validate_directory(bytes, sections);

        if (!result.ok()) {
            return result;
        }

        compiled_project_state compiled;

        compiled.graph.abi.target =
            static_cast<abi_target>(
                read32(bytes.data() + 40));

        compiled.graph.abi.pack =
            read32(bytes.data() + 44);

        if (!is_supported_abi_configuration(compiled.graph.abi)) {
            return {status_code::artifact_corrupt};
        }

        result = decode_strings(
            bytes,
            sections,
            compiled);

        if (!result.ok()) {
            return result;
        }

        result = decode_graph_sections(
            bytes,
            sections,
            compiled);

        if (!result.ok()) {
            return result;
        }

        result = validate_cross_references(compiled);

        if (!result.ok()) {
            return result;
        }

        if (output) {
            *output = std::move(compiled);
        }

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status read_all(
    const std::filesystem::path& path,
    std::vector<std::byte>& bytes) {

    std::ifstream file(
        path,
        std::ios::binary | std::ios::ate);

    if (!file) {
        return {status_code::persistence_failed};
    }

    const auto position = file.tellg();

    if (position < 0) {
        return {status_code::persistence_failed};
    }

    const auto size =
        static_cast<std::uintmax_t>(position);

    if (size >
        (std::numeric_limits<std::size_t>::max)()) {
        return {status_code::initialization_failed};
    }

    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0);

    if (!bytes.empty() &&
        !file.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        return {status_code::persistence_failed};
    }

    return {};
}

// Owns one native writable file handle used during transactional publication.
#ifdef _WIN32
using native_handle = HANDLE;
constexpr native_handle invalid_native_handle = INVALID_HANDLE_VALUE;
#else
using native_handle = int;
constexpr native_handle invalid_native_handle = -1;
#endif

struct unique_handle {
    native_handle value = invalid_native_handle;

    unique_handle() noexcept = default;

    explicit unique_handle(native_handle handle) noexcept
        : value(handle) {}

    ~unique_handle() {
        close();
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    [[nodiscard]] bool valid() const noexcept {
#ifdef _WIN32
        return
            value != INVALID_HANDLE_VALUE &&
            value != nullptr;
#else
        return value >= 0;
#endif
    }

    void close() noexcept {
        if (!valid()) {
            return;
        }

#ifdef _WIN32
        CloseHandle(value);
#else
        ::close(value);
#endif

        value = invalid_native_handle;
    }
};

std::filesystem::path temporary_checkpoint_path(
    const std::filesystem::path& target) {

    auto temporary = target;

#ifdef _WIN32
    temporary +=
        L".tmp." +
        std::to_wstring(GetCurrentProcessId()) +
        L"." +
        std::to_wstring(GetTickCount64());
#else
    const auto stamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            persistence_clock::now().time_since_epoch()).count();

    temporary +=
        ".tmp." +
        std::to_string(
            static_cast<unsigned long long>(::getpid())) +
        "." +
        std::to_string(
            static_cast<unsigned long long>(stamp));
#endif

    return temporary;
}

status create_new_write_file(
    const std::filesystem::path& path,
    unique_handle& output) noexcept {

#ifdef _WIN32
    output.value = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    output.value = ::open(
        path.c_str(),
        flags,
        static_cast<mode_t>(0666));
#endif

    return
        output.valid()
            ? status{}
            : status{status_code::persistence_failed};
}

status write_all(
    native_handle file,
    std::span<const std::byte> bytes) noexcept {

    std::size_t written = 0;

    while (written < bytes.size()) {
        const auto remaining = bytes.size() - written;

#ifdef _WIN32
        const auto request =
            static_cast<DWORD>(
                (std::min)(
                    remaining,
                    std::size_t(
                        (std::numeric_limits<DWORD>::max)())));

        DWORD amount = 0;

        if (!WriteFile(
                file,
                bytes.data() + written,
                request,
                &amount,
                nullptr) ||
            amount == 0) {
            return {status_code::persistence_failed};
        }

        written += amount;
#else
        const auto request =
            (std::min)(
                remaining,
                static_cast<std::size_t>(
                    (std::numeric_limits<ssize_t>::max)()));

        const auto amount = ::write(
            file,
            bytes.data() + written,
            request);

        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }

            return {status_code::persistence_failed};
        }

        if (amount == 0) {
            return {status_code::persistence_failed};
        }

        written += static_cast<std::size_t>(amount);
#endif
    }

    return {};
}

status flush_file(native_handle file) noexcept {
#ifdef _WIN32
    return
        FlushFileBuffers(file)
            ? status{}
            : status{status_code::persistence_failed};
#else
    while (::fsync(file) != 0) {
        if (errno == EINTR) {
            continue;
        }

        return {status_code::persistence_failed};
    }

    return {};
#endif
}

void delete_file(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    DeleteFileW(path.c_str());
#else
    ::unlink(path.c_str());
#endif
}

#ifndef _WIN32
status sync_parent_directory(
    const std::filesystem::path& target) noexcept {

    auto parent = target.parent_path();

    if (parent.empty()) {
        parent = ".";
    }

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif

    unique_handle directory{
        ::open(parent.c_str(), flags)
    };

    if (!directory.valid()) {
        return {status_code::persistence_failed};
    }

    while (::fsync(directory.value) != 0) {
        if (errno == EINTR) {
            continue;
        }

        return {status_code::persistence_failed};
    }

    return {};
}
#endif

status publish_checkpoint(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target) noexcept {

#ifdef _WIN32
    const DWORD attributes =
        GetFileAttributesW(target.c_str());

    const BOOL published =
        attributes != INVALID_FILE_ATTRIBUTES
            ? ReplaceFileW(
                  target.c_str(),
                  temporary.c_str(),
                  nullptr,
                  REPLACEFILE_WRITE_THROUGH,
                  nullptr,
                  nullptr)
            : MoveFileExW(
                  temporary.c_str(),
                  target.c_str(),
                  MOVEFILE_WRITE_THROUGH);

    return
        published
            ? status{}
            : status{status_code::persistence_failed};
#else
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        return {status_code::persistence_failed};
    }

    return sync_parent_directory(target);
#endif
}

void write_header_and_directory(
    std::vector<std::byte>& bytes,
    const compiled_project_state& compiled,
    const std::array<section_descriptor, section_count>& sections) noexcept {

    std::memcpy(
        bytes.data(),
        magic.data(),
        magic.size());

    write16(bytes.data() + 8, format_major);
    write16(bytes.data() + 10, format_minor);
    write32(bytes.data() + 12, header_size);
    write32(bytes.data() + 16, endian_marker);
    write64(bytes.data() + 24, bytes.size());
    write32(bytes.data() + 32, directory_offset);
    write32(bytes.data() + 36, section_count);
    write32(
        bytes.data() + 40,
        static_cast<std::uint32_t>(
            compiled.graph.abi.target));
    write32(
        bytes.data() + 44,
        compiled.graph.abi.pack);

    for (std::size_t index = 0;
         index != section_count;
         ++index) {
        auto* entry =
            bytes.data() +
            directory_offset +
            index * section_entry_size;

        const auto& section = sections[index];

        write32(entry, section.kind);
        write32(entry + 4, 0);
        write64(entry + 8, section.offset);
        write64(entry + 16, section.bytes);
        write32(entry + 24, section.count);
        write32(entry + 28, section.record_size);
    }
}

void write_strings(
    std::vector<std::byte>& bytes,
    const compiled_project_state& compiled,
    const std::array<section_descriptor, section_count>& sections) noexcept {

    const auto& records =
        sections[to_index(section_index::strings)];

    const auto& storage =
        sections[to_index(section_index::string_bytes)];

    std::uint64_t storage_offset = 0;

    for (std::size_t index = 0;
         index != compiled.strings.size();
         ++index) {
        auto* record =
            bytes.data() +
            records.offset +
            std::uint64_t(index) * records.record_size;

        const auto& value = compiled.strings[index];

        if (!value) {
            write32(
                record,
                (std::numeric_limits<std::uint32_t>::max)());
            write32(record + 4, 0);
            continue;
        }

        write32(
            record,
            static_cast<std::uint32_t>(storage_offset));

        write32(
            record + 4,
            static_cast<std::uint32_t>(value->size()));

        if (!value->empty()) {
            std::memcpy(
                bytes.data() +
                    storage.offset +
                    storage_offset,
                value->data(),
                value->size());
        }

        storage_offset += value->size();
    }
}

void write_graph_sections(
    std::vector<std::byte>& bytes,
    const compiled_project_state& compiled,
    const std::array<section_descriptor, section_count>& sections) noexcept {

    const auto& identities = sections[to_index(section_index::identities)];

    for (std::size_t index = 0; index != compiled.graph.identities.size(); ++index) {
        write32(
            bytes.data() + identities.offset +
                std::uint64_t(index) * identities.record_size,
            compiled.graph.identities[index]);
    }

    const auto& entities = sections[to_index(section_index::entities)];

    for (std::size_t index = 0; index != compiled.graph.entities.size(); ++index) {
        auto* record = bytes.data() + entities.offset +
            std::uint64_t(index) * entities.record_size;

        const auto& entity = compiled.graph.entities[index];

        record[0] = std::byte(entity.kind);
        record[1] = std::byte{0};
        write16(record + 2, 0);
        write32(record + 4, entity.name);
        write32(record + 8, entity.type);
        write32(record + 12, 0);
    }

    const auto& types = sections[to_index(section_index::types)];

    for (std::size_t index = 0; index != compiled.graph.types.size(); ++index) {
        auto* record = bytes.data() + types.offset +
            std::uint64_t(index) * types.record_size;

        const auto& type = compiled.graph.types[index];

        record[0] = std::byte(type.live ? 1 : 0);

        if (!type.live) {
            record[1] = std::byte{0};
            record[2] = std::byte{0};
            record[3] = std::byte{0};
            record[4] = std::byte{0};
            record[5] = std::byte{0};
            write16(record + 6, 0);
            write32(record + 8, 0);
            write32(record + 12, 0);
            continue;
        }

        record[1] = std::byte(type.record.kind);

        if (type.record.kind == user_type_kind::enumeration) {
            record[2] = std::byte(type.record.enumeration.scoped ? 1 : 0);
            record[3] = std::byte(type.record.enumeration.fixed_underlying ? 1 : 0);
            record[4] = std::byte(type.record.enumeration.underlying);
        }
        else {
            record[2] = std::byte{0};
            record[3] = std::byte{0};
            record[4] = std::byte{0};
        }

        record[5] = std::byte{0};
        write16(record + 6, 0);
        write32(record + 8, type.record.definition.begin);
        write32(record + 12, type.record.definition.count);
    }

    const auto& enum_values = sections[to_index(section_index::enum_values)];

    for (std::size_t index = 0; index != compiled.graph.enum_values.size(); ++index) {
        auto* record = bytes.data() + enum_values.offset +
            std::uint64_t(index) * enum_values.record_size;

        const auto& value = compiled.graph.enum_values[index];
        write32(record, value.name);
        write32(record + 4, 0);
        write64(record + 8, value.bits);
    }

    const auto& members = sections[to_index(section_index::members)];

    for (std::size_t index = 0; index != compiled.graph.members.size(); ++index) {
        auto* record = bytes.data() + members.offset +
            std::uint64_t(index) * members.record_size;

        const auto& member = compiled.graph.members[index];
        write32(record, member.name);
        write32(record + 4, member.type_ref);
    }

    const auto& canonical_types = sections[to_index(section_index::canonical_types)];

    for (std::size_t index = 0; index != compiled.graph.canonical_types.size(); ++index) {
        auto* record = bytes.data() + canonical_types.offset +
            std::uint64_t(index) * canonical_types.record_size;

        const auto& type = compiled.graph.canonical_types[index];
        record[0] = std::byte(type.kind);
        record[1] = std::byte(type.subtype);
        write16(record + 2, 0);
        write32(record + 4, type.argument);
        write64(record + 8, type.payload);
    }
}

void seal_artifact(std::vector<std::byte>& bytes) noexcept {
    const auto digest = core::sha256({
        reinterpret_cast<const char*>(
            bytes.data() + header_size),
        bytes.size() - header_size
    });

    std::memcpy(
        bytes.data() + payload_hash_offset,
        digest.data(),
        digest.size());

    write32(bytes.data() + header_crc_offset, 0);

    write32(
        bytes.data() + header_crc_offset,
        crc32c({bytes.data(), header_size}));
}

} // namespace

status write_compiled_checkpoint(
    const std::filesystem::path& path,
    const string_registry& registry,
    const graph& graph_state,
    metrics_store* metrics) noexcept {

    const auto total_begin = persistence_clock::now();

    try {
        compiled_project_state compiled;

        auto phase_begin = persistence_clock::now();

        auto result =
            registry.export_slots(compiled.strings);

        if (result.ok()) {
            result =
                graph_state.export_compiled(compiled.graph);
        }

        if (!result.ok()) {
            return result;
        }

        std::array<section_descriptor, section_count> sections{};
        std::uint64_t artifact_size = 0;

        result = build_sections(
            compiled,
            sections,
            artifact_size);

        if (!result.ok()) {
            return result;
        }

        if (artifact_size >
            (std::numeric_limits<std::size_t>::max)()) {
            return {status_code::initialization_failed};
        }

        std::vector<std::byte> bytes(
            static_cast<std::size_t>(artifact_size));

        write_header_and_directory(
            bytes,
            compiled,
            sections);

        write_strings(
            bytes,
            compiled,
            sections);

        write_graph_sections(
            bytes,
            compiled,
            sections);

        record_duration(
            metrics,
            metric_id::compiled_save_materialization_duration,
            phase_begin);

        phase_begin = persistence_clock::now();
        seal_artifact(bytes);

        record_duration(
            metrics,
            metric_id::compiled_save_hash_duration,
            phase_begin);

        const auto temporary =
            temporary_checkpoint_path(path);

        unique_handle file;

        phase_begin = persistence_clock::now();

        result = create_new_write_file(
            temporary,
            file);

        if (result.ok()) {
            result = write_all(file.value, bytes);
        }

        record_duration(
            metrics,
            metric_id::compiled_save_write_duration,
            phase_begin);

        if (!result.ok()) {
            file.close();
            delete_file(temporary);
            return result;
        }

        phase_begin = persistence_clock::now();
        result = flush_file(file.value);

        record_duration(
            metrics,
            metric_id::compiled_save_flush_duration,
            phase_begin);

        file.close();

        if (!result.ok()) {
            delete_file(temporary);
            return result;
        }

        // Reopen and decode the complete candidate artifact before publication.
        // This validates the same bytes a subsequent loader will observe.
        std::vector<std::byte> verification;
        result = read_all(temporary, verification);

        if (result.ok()) {
            result = decode_artifact(verification, nullptr);
        }

        if (!result.ok()) {
            delete_file(temporary);
            return result;
        }

        result = publish_checkpoint(
            temporary,
            path);

        if (!result.ok()) {
            delete_file(temporary);
            return result;
        }

        if (metrics) {
            metrics->increment(
                metric_id::compiled_artifact_bytes,
                bytes.size());

            metrics->increment(
                metric_id::compiled_string_count,
                compiled.strings.size());

            metrics->increment(
                metric_id::compiled_entity_slot_count,
                compiled.graph.entities.size());

            metrics->increment(
                metric_id::compiled_type_slot_count,
                compiled.graph.types.size());

            metrics->increment(
                metric_id::compiled_enum_value_count,
                sections[to_index(section_index::enum_values)].count);
        }

        record_duration(
            metrics,
            metric_id::compiled_save_total_duration,
            total_begin);

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

status read_compiled_checkpoint(
    const std::filesystem::path& path,
    string_registry& registry,
    graph& graph_state,
    metrics_store* metrics) noexcept {

    const auto total_begin = persistence_clock::now();

    try {
        std::vector<std::byte> bytes;

        auto phase_begin = persistence_clock::now();
        auto result = read_all(path, bytes);

        record_duration(
            metrics,
            metric_id::compiled_load_open_duration,
            phase_begin);

        if (!result.ok()) {
            return result;
        }

        compiled_project_state compiled;

        phase_begin = persistence_clock::now();
        result = decode_artifact(bytes, &compiled);

        record_duration(
            metrics,
            metric_id::compiled_load_validate_duration,
            phase_begin);

        if (!result.ok()) {
            return result;
        }

        phase_begin = persistence_clock::now();
        result = registry.import_slots(compiled.strings);

        record_duration(
            metrics,
            metric_id::compiled_load_strings_duration,
            phase_begin);

        if (!result.ok()) {
            return result;
        }

        phase_begin = persistence_clock::now();
        result = graph_state.import_compiled(compiled.graph);

        record_duration(
            metrics,
            metric_id::compiled_load_graph_duration,
            phase_begin);

        if (!result.ok()) {
            return result;
        }

        if (metrics) {
            metrics->increment(
                metric_id::compiled_artifact_bytes,
                bytes.size());
        }

        record_duration(
            metrics,
            metric_id::compiled_load_total_duration,
            total_begin);

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

} // namespace cw::server
