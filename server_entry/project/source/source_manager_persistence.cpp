#include "source_manager_persistence.hpp"

#include "../../core/hash/sha256.hpp"
#include "../../metrics/metrics_store.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cw::server {
namespace {

using persistence_clock = std::chrono::steady_clock;

// Frozen on-disk container contract.
constexpr std::size_t header_size = 128;
constexpr std::size_t directory_offset = 128;
constexpr std::size_t section_count = 9;
constexpr std::size_t section_entry_size = 32;
constexpr std::size_t directory_size = section_count * section_entry_size;

constexpr std::uint16_t format_major = 1;
constexpr std::uint16_t format_minor = 0;
constexpr std::uint32_t container_kind = 1;
constexpr std::uint32_t endian_marker = 0x01020304;

constexpr std::size_t payload_hash_offset = 72;
constexpr std::size_t header_crc_offset = 104;
constexpr std::size_t reserved_header_offset = 108;

constexpr std::array<char, 8> magic{
    'C', 'W', 'S', 'M', 'B', 'I', 'N', '1'
};

enum class section_index : std::size_t {
    source_core = 0,
    physical_state,
    forward_offsets,
    forward_edges,
    reverse_offsets,
    reverse_edges,
    roots,
    path_index,
    path_bytes
};

constexpr std::size_t to_index(section_index section) noexcept {
    return static_cast<std::size_t>(section);
}

constexpr std::array<std::uint32_t, section_count> section_element_sizes{
    8, 56, 4, 4, 4, 4, 8, 8, 1
};

constexpr std::array<std::uint32_t, section_count> section_alignments{
    4, 8, 4, 4, 4, 4, 4, 4, 1
};

// Records one checkpoint save attempt, total duration, and final outcome.
struct save_metric_scope {
    metrics_store* store = nullptr;
    persistence_clock::time_point start = persistence_clock::now();
    bool success = false;

    explicit save_metric_scope(metrics_store* value) noexcept
        : store(value) {
        if (store) {
            store->increment(
                metric_id::source_checkpoint_save_attempt_count);
        }
    }

    ~save_metric_scope() {
        if (!store) {
            return;
        }

        store->record_duration(
            metric_id::source_checkpoint_save_duration,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                persistence_clock::now() - start));

        store->increment(
            success
                ? metric_id::source_checkpoint_save_success_count
                : metric_id::source_checkpoint_save_failure_count);
    }
};

void record_phase(
    metrics_store* store,
    metric_id id,
    persistence_clock::time_point begin) noexcept {

    if (!store) {
        return;
    }

    store->record_duration(
        id,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            persistence_clock::now() - begin));
}

std::uint16_t read16(const std::byte* data) noexcept {
    return
        std::uint16_t(std::to_integer<unsigned char>(data[0])) |
        (std::uint16_t(std::to_integer<unsigned char>(data[1])) << 8);
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

void write16(std::byte* data, std::uint16_t value) noexcept {
    for (unsigned index = 0; index != 2; ++index) {
        data[index] = std::byte((value >> (index * 8)) & 0xff);
    }
}

void write32(std::byte* data, std::uint32_t value) noexcept {
    for (unsigned index = 0; index != 4; ++index) {
        data[index] = std::byte((value >> (index * 8)) & 0xff);
    }
}

void write64(std::byte* data, std::uint64_t value) noexcept {
    for (unsigned index = 0; index != 8; ++index) {
        data[index] = std::byte((value >> (index * 8)) & 0xff);
    }
}

std::uint64_t align_up(
    std::uint64_t value,
    std::uint64_t alignment,
    bool& ok) noexcept {

    const auto mask = alignment - 1;

    if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
        ok = false;
        return 0;
    }

    return (value + mask) & ~mask;
}

// Owns one native file or mapping handle.
#ifdef _WIN32
using native_handle = HANDLE;
const native_handle invalid_native_handle = INVALID_HANDLE_VALUE;
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

    [[nodiscard]] bool valid() const noexcept {
#ifdef _WIN32
        return value != INVALID_HANDLE_VALUE && value != nullptr;
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

// Owns one read-only memory mapping.
struct unique_mapping_view {
    void* data = nullptr;
    std::size_t size = 0;

    ~unique_mapping_view() {
        reset();
    }

    void reset() noexcept {
        if (!data) {
            return;
        }

#ifdef _WIN32
        UnmapViewOfFile(data);
#else
        ::munmap(data, size);
#endif
        data = nullptr;
        size = 0;
    }
};

struct mapped_file {
    unique_handle file;
#ifdef _WIN32
    unique_handle mapping;
#endif
    unique_mapping_view view;
    std::uint64_t size = 0;
};

status map_readonly(
    const std::filesystem::path& path,
    mapped_file& output) noexcept {

#ifdef _WIN32
    output.file.value = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (!output.file.valid()) {
        return {status_code::persistence_failed};
    }

    LARGE_INTEGER file_size{};

    if (!GetFileSizeEx(output.file.value, &file_size) ||
        file_size.QuadPart < 0) {
        return {status_code::persistence_failed};
    }

    output.size = static_cast<std::uint64_t>(file_size.QuadPart);

    output.mapping.value = CreateFileMappingW(
        output.file.value,
        nullptr,
        PAGE_READONLY,
        0,
        0,
        nullptr);

    if (!output.mapping.valid()) {
        return {status_code::persistence_failed};
    }

    output.view.data = MapViewOfFile(
        output.mapping.value,
        FILE_MAP_READ,
        0,
        0,
        0);

    if (!output.view.data) {
        return {status_code::persistence_failed};
    }

    output.view.size = static_cast<std::size_t>(output.size);
    return {};
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    output.file.value = ::open(path.c_str(), flags);

    if (!output.file.valid()) {
        return {status_code::persistence_failed};
    }

    struct stat information {};

    if (::fstat(output.file.value, &information) != 0 ||
        information.st_size < 0) {
        return {status_code::persistence_failed};
    }

    const auto file_size =
        static_cast<std::uint64_t>(information.st_size);

    if (file_size == 0 ||
        file_size > std::numeric_limits<std::size_t>::max()) {
        return {status_code::persistence_failed};
    }

    output.size = file_size;
    output.view.size = static_cast<std::size_t>(file_size);

    void* mapping = ::mmap(
        nullptr,
        output.view.size,
        PROT_READ,
        MAP_PRIVATE,
        output.file.value,
        0);

    if (mapping == MAP_FAILED) {
        output.view.size = 0;
        return {status_code::persistence_failed};
    }

    output.view.data = mapping;
    return {};
#endif
}

struct section_view {
    const std::byte* data = nullptr;
    std::uint64_t size = 0;
    std::uint32_t count = 0;
    std::uint32_t element_size = 0;
};

// Describes validated zero-copy views over one mapped checkpoint.
struct decoded_file {
    const std::byte* base = nullptr;
    std::uint64_t file_size = 0;

    std::uint32_t sources = 0;
    std::uint32_t roots = 0;
    std::uint32_t forward_edges = 0;
    std::uint32_t reverse_edges = 0;
    std::uint32_t index_capacity = 0;
    std::uint32_t path_bytes = 0;

    std::array<section_view, section_count> sections{};
};

const section_view& section(
    const decoded_file& file,
    section_index index) noexcept {

    return file.sections[to_index(index)];
}

status validate_header(
    const std::byte* base,
    std::uint64_t actual_size) noexcept {

    if (!base || actual_size < header_size + directory_size) {
        return {status_code::artifact_corrupt};
    }

    const bool fixed_fields_valid =
        std::memcmp(base, magic.data(), magic.size()) == 0 &&
        read16(base + 8) == format_major &&
        read16(base + 10) == format_minor &&
        read32(base + 12) == header_size &&
        read32(base + 16) == container_kind &&
        read32(base + 20) == endian_marker &&
        read64(base + 24) == actual_size &&
        read32(base + 56) == section_count &&
        read32(base + 60) == section_entry_size &&
        read64(base + 64) == directory_offset;

    if (!fixed_fields_valid) {
        return {status_code::artifact_corrupt};
    }

    std::array<std::byte, header_size> header{};
    std::memcpy(header.data(), base, header.size());

    const auto stored_crc =
        read32(header.data() + header_crc_offset);

    std::fill(
        header.begin() + header_crc_offset,
        header.begin() + header_crc_offset + sizeof(std::uint32_t),
        std::byte{});

    if (source_manager_crc32c(header) != stored_crc) {
        return {status_code::artifact_corrupt};
    }

    for (std::size_t index = reserved_header_offset;
         index != header_size;
         ++index) {
        if (base[index] != std::byte{}) {
            return {status_code::artifact_corrupt};
        }
    }

    return {};
}

void decode_header_counts(
    const std::byte* base,
    std::uint64_t actual_size,
    decoded_file& output) noexcept {

    output.base = base;
    output.file_size = actual_size;
    output.sources = read32(base + 32);
    output.roots = read32(base + 36);
    output.forward_edges = read32(base + 40);
    output.reverse_edges = read32(base + 44);
    output.index_capacity = read32(base + 48);
    output.path_bytes = read32(base + 52);
}

std::array<std::uint32_t, section_count> expected_section_counts(
    const decoded_file& file) noexcept {

    return {
        file.sources,
        file.sources,
        file.sources + 1,
        file.forward_edges,
        file.sources + 1,
        file.reverse_edges,
        file.roots,
        file.index_capacity,
        file.path_bytes
    };
}

status decode_sections(
    const std::byte* base,
    std::uint64_t actual_size,
    decoded_file& output) noexcept {

    const auto expected_counts = expected_section_counts(output);
    std::uint64_t previous_end = header_size + directory_size;

    for (std::size_t index = 0; index != section_count; ++index) {
        const auto* entry =
            base + directory_offset + index * section_entry_size;

        const auto kind = read32(entry);
        const auto flags = read32(entry + 4);
        const auto offset = read64(entry + 8);
        const auto byte_size = read64(entry + 16);
        const auto count = read32(entry + 24);
        const auto element_size = read32(entry + 28);

        const bool entry_valid =
            kind == index + 1 &&
            flags == 0 &&
            element_size == section_element_sizes[index] &&
            count == expected_counts[index] &&
            byte_size == std::uint64_t(count) * element_size &&
            offset % section_alignments[index] == 0 &&
            offset >= previous_end &&
            offset <= actual_size &&
            byte_size <= actual_size - offset;

        if (!entry_valid) {
            return {status_code::artifact_corrupt};
        }

        for (auto padding = previous_end;
             padding != offset;
             ++padding) {
            if (base[padding] != std::byte{}) {
                return {status_code::artifact_corrupt};
            }
        }

        output.sections[index] = {
            base + offset,
            byte_size,
            count,
            element_size
        };

        previous_end = offset + byte_size;
    }

    if (previous_end != actual_size) {
        return {status_code::artifact_corrupt};
    }

    return {};
}

status validate_container_shape(
    const decoded_file& file) noexcept {

    if (file.sources == std::numeric_limits<std::uint32_t>::max()) {
        return {status_code::artifact_corrupt};
    }

    if (file.sources == 0) {
        if (file.roots != 0 ||
            file.forward_edges != 0 ||
            file.reverse_edges != 0 ||
            file.index_capacity != 0 ||
            file.path_bytes != 0) {
            return {status_code::artifact_corrupt};
        }

        return {};
    }

    const bool index_valid =
        file.index_capacity != 0 &&
        std::has_single_bit(file.index_capacity) &&
        std::uint64_t(file.sources) * 10 <=
            std::uint64_t(file.index_capacity) * 7;

    return index_valid
               ? status{}
               : status{status_code::artifact_corrupt};
}

// Validates the fixed container/header/directory layout and establishes safe
// zero-copy section views. Deep payload semantics are checked separately.
status decode_container(
    const std::byte* base,
    std::uint64_t actual_size,
    decoded_file& output) noexcept {

    auto result = validate_header(base, actual_size);

    if (!result.ok()) {
        return result;
    }

    decode_header_counts(base, actual_size, output);

    result = validate_container_shape(output);

    if (!result.ok()) {
        return result;
    }

    return decode_sections(base, actual_size, output);
}

status checked_path_bytes(
    const decoded_file& file,
    source_id id,
    std::string_view& output) noexcept {

    output = {};

    if (!id || id.value() > file.sources) {
        return {status_code::artifact_corrupt};
    }

    const auto& source_core =
        section(file, section_index::source_core);

    const auto& path_bytes =
        section(file, section_index::path_bytes);

    const auto* record =
        source_core.data + std::uint64_t(id.value() - 1) * 8;

    const auto offset = read32(record);
    const auto length = read32(record + 4);

    if (offset > file.path_bytes ||
        length > file.path_bytes - offset) {
        return {status_code::artifact_corrupt};
    }

    output = {
        reinterpret_cast<const char*>(path_bytes.data + offset),
        length
    };

    return {};
}

status checked_dependencies(
    const decoded_file& file,
    source_id id,
    bool reverse,
    std::vector<source_id>& output) noexcept {

    output.clear();

    if (!id || id.value() > file.sources) {
        return {status_code::artifact_corrupt};
    }

    const auto& offsets =
        section(
            file,
            reverse
                ? section_index::reverse_offsets
                : section_index::forward_offsets);

    const auto& edges =
        section(
            file,
            reverse
                ? section_index::reverse_edges
                : section_index::forward_edges);

    const auto begin =
        read32(
            offsets.data +
            std::uint64_t(id.value() - 1) * sizeof(std::uint32_t));

    const auto end =
        read32(
            offsets.data +
            std::uint64_t(id.value()) * sizeof(std::uint32_t));

    if (begin > end || end > edges.count) {
        return {status_code::artifact_corrupt};
    }

    try {
        output.reserve(end - begin);
    }
    catch (...) {
        return {status_code::initialization_failed};
    }

    for (auto index = begin; index != end; ++index) {
        const auto value =
            read32(
                edges.data +
                std::uint64_t(index) * sizeof(std::uint32_t));

        if (value == 0 || value > file.sources) {
            output.clear();
            return {status_code::artifact_corrupt};
        }

        output.push_back(source_id{value});
    }

    return {};
}

std::uint64_t xx_round(
    std::uint64_t accumulator,
    std::uint64_t input) noexcept {

    constexpr std::uint64_t prime1 = 11400714785074694791ull;
    constexpr std::uint64_t prime2 = 14029467366897019727ull;

    accumulator += input * prime2;
    accumulator = std::rotl(accumulator, 31);
    return accumulator * prime1;
}

status validate_payload_hash(const decoded_file& file) noexcept {
    const auto payload_size =
        static_cast<std::size_t>(file.file_size - header_size);

    const auto digest =
        core::sha256({
            reinterpret_cast<const char*>(file.base + header_size),
            payload_size
        });

    return std::memcmp(
               digest.data(),
               file.base + payload_hash_offset,
               digest.size()) == 0
               ? status{}
               : status{status_code::artifact_corrupt};
}

// Checkpoint path bytes preserve the native filesystem representation.
// Windows uses canonical WTF-8 for UTF-16 paths; POSIX stores native bytes
// directly so non-UTF-8 filenames round-trip without loss.
status encode_checkpoint_path(
    const std::filesystem::path& path,
    std::string& output) noexcept {

#ifdef _WIN32
    return encode_wtf8(path.native(), output);
#else
    output.clear();

    try {
        const auto& native = path.native();

        if (native.find('\0') != std::string::npos) {
            return {status_code::configuration_failed};
        }

        output.assign(native.data(), native.size());
        return {};
    }
    catch (...) {
        output.clear();
        return {status_code::initialization_failed};
    }
#endif
}

status decode_checkpoint_path(
    std::string_view bytes,
    std::filesystem::path& output) noexcept {

    output.clear();

#ifdef _WIN32
    std::wstring decoded;
    const auto result = decode_wtf8(bytes, decoded);

    if (!result.ok()) {
        return result;
    }

    try {
        output = std::filesystem::path{decoded};
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
#else
    if (bytes.find('\0') != std::string_view::npos) {
        return {status_code::artifact_corrupt};
    }

    try {
        output = std::filesystem::path{
            std::string{bytes.data(), bytes.size()}};
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
#endif
}

status validate_paths_and_physical(const decoded_file& file) noexcept {
    std::unordered_set<std::string_view> paths;
    std::uint32_t expected_offset = 0;

    try {
        paths.reserve(file.sources);
    }
    catch (...) {
        return {status_code::initialization_failed};
    }

    const auto& source_core =
        section(file, section_index::source_core);

    const auto& physical =
        section(file, section_index::physical_state);

    for (std::uint32_t value = 1;
         value <= file.sources;
         ++value) {
        const auto id = source_id{value};

        std::string_view bytes;
        auto result = checked_path_bytes(file, id, bytes);

        if (!result.ok()) {
            return result;
        }

        const auto* source_record =
            source_core.data + std::uint64_t(value - 1) * 8;

        if (read32(source_record) != expected_offset ||
            bytes.empty()) {
            return {status_code::artifact_corrupt};
        }

        if (bytes.size() >
            std::numeric_limits<std::uint32_t>::max() -
                expected_offset) {
            return {status_code::artifact_corrupt};
        }

        expected_offset +=
            static_cast<std::uint32_t>(bytes.size());

        std::filesystem::path decoded;
        result = decode_checkpoint_path(bytes, decoded);

        if (!result.ok() ||
            !paths.insert(bytes).second) {
            return {status_code::artifact_corrupt};
        }

        const auto* physical_record =
            physical.data + std::uint64_t(value - 1) * 56;

        const auto flags = read32(physical_record + 48);
        const auto reserved = read32(physical_record + 52);

        if ((flags & ~1u) != 0 || reserved != 0) {
            return {status_code::artifact_corrupt};
        }

        if ((flags & 1u) == 0) {
            for (std::size_t byte = 0; byte != 48; ++byte) {
                if (physical_record[byte] != std::byte{}) {
                    return {status_code::artifact_corrupt};
                }
            }
        }
    }

    return expected_offset == file.path_bytes
               ? status{}
               : status{status_code::artifact_corrupt};
}

status validate_csr(
    const decoded_file& file,
    bool reverse) noexcept {

    const auto& offsets =
        section(
            file,
            reverse
                ? section_index::reverse_offsets
                : section_index::forward_offsets);

    const auto& edges =
        section(
            file,
            reverse
                ? section_index::reverse_edges
                : section_index::forward_edges);

    if (read32(offsets.data) != 0 ||
        read32(
            offsets.data +
            std::uint64_t(file.sources) *
                sizeof(std::uint32_t)) != edges.count) {
        return {status_code::artifact_corrupt};
    }

    for (std::uint32_t value = 1;
         value <= file.sources;
         ++value) {
        const auto begin =
            read32(
                offsets.data +
                std::uint64_t(value - 1) *
                    sizeof(std::uint32_t));

        const auto end =
            read32(
                offsets.data +
                std::uint64_t(value) *
                    sizeof(std::uint32_t));

        if (begin > end || end > edges.count) {
            return {status_code::artifact_corrupt};
        }

        for (auto edge_index = begin;
             edge_index != end;
             ++edge_index) {
            const auto target =
                read32(
                    edges.data +
                    std::uint64_t(edge_index) *
                        sizeof(std::uint32_t));

            if (target == 0 || target > file.sources) {
                return {status_code::artifact_corrupt};
            }
        }
    }

    return {};
}

status validate_roots(const decoded_file& file) noexcept {
    const auto& roots = section(file, section_index::roots);

    for (std::uint32_t index = 0;
         index != file.roots;
         ++index) {
        const auto* record =
            roots.data + std::uint64_t(index) * 8;

        const auto source = read32(record);
        const auto role =
            std::to_integer<unsigned char>(record[4]);

        if (source == 0 ||
            source > file.sources ||
            role > 1) {
            return {status_code::artifact_corrupt};
        }

        if (record[5] != std::byte{} ||
            record[6] != std::byte{} ||
            record[7] != std::byte{}) {
            return {status_code::artifact_corrupt};
        }
    }

    return {};
}

status validate_path_index(const decoded_file& file) noexcept {
    std::vector<std::uint32_t> seen(file.sources + 1);
    const auto& index =
        section(file, section_index::path_index);

    for (std::uint32_t bucket_index = 0;
         bucket_index != file.index_capacity;
         ++bucket_index) {
        const auto* bucket =
            index.data + std::uint64_t(bucket_index) * 8;

        const auto fingerprint = read32(bucket);
        const auto id_value = read32(bucket + 4);

        if (id_value == 0) {
            if (fingerprint != 0) {
                return {status_code::artifact_corrupt};
            }

            continue;
        }

        if (id_value > file.sources ||
            fingerprint == 0 ||
            seen[id_value]++ != 0) {
            return {status_code::artifact_corrupt};
        }

        std::string_view bytes;
        auto result =
            checked_path_bytes(
                file,
                source_id{id_value},
                bytes);

        if (!result.ok()) {
            return result;
        }

        const auto hash = source_path_xxh64(bytes);

        if (source_path_fingerprint(hash) != fingerprint) {
            return {status_code::artifact_corrupt};
        }

        auto probe =
            std::uint32_t(hash) &
            (file.index_capacity - 1);

        bool reachable = false;

        for (std::uint32_t count = 0;
             count != file.index_capacity;
             ++count,
             probe = (probe + 1) &
                     (file.index_capacity - 1)) {
            const auto* probe_bucket =
                index.data + std::uint64_t(probe) * 8;

            if (read32(probe_bucket + 4) == 0) {
                break;
            }

            if (probe == bucket_index) {
                reachable = true;
                break;
            }
        }

        if (!reachable) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::uint32_t id = 1;
         id <= file.sources;
         ++id) {
        if (seen[id] != 1) {
            return {status_code::artifact_corrupt};
        }
    }

    return {};
}

status validate_dependency_symmetry(
    const decoded_file& file) noexcept {

    if (file.forward_edges != file.reverse_edges) {
        return {status_code::artifact_corrupt};
    }

    std::unordered_set<std::uint64_t> forward;

    try {
        forward.reserve(file.forward_edges);
    }
    catch (...) {
        return {status_code::initialization_failed};
    }

    for (std::uint32_t parent = 1;
         parent <= file.sources;
         ++parent) {
        std::vector<source_id> children;

        auto result =
            checked_dependencies(
                file,
                source_id{parent},
                false,
                children);

        if (!result.ok()) {
            return result;
        }

        for (auto child : children) {
            const auto edge =
                (std::uint64_t(parent) << 32) |
                child.value();

            if (!forward.insert(edge).second) {
                return {status_code::artifact_corrupt};
            }
        }
    }

    for (std::uint32_t child = 1;
         child <= file.sources;
         ++child) {
        std::vector<source_id> parents;

        auto result =
            checked_dependencies(
                file,
                source_id{child},
                true,
                parents);

        if (!result.ok()) {
            return result;
        }

        for (auto parent : parents) {
            const auto edge =
                (std::uint64_t(parent.value()) << 32) |
                child;

            if (forward.erase(edge) == 0) {
                return {status_code::artifact_corrupt};
            }
        }
    }

    return forward.empty()
               ? status{}
               : status{status_code::artifact_corrupt};
}

status read_root(
    const decoded_file& file,
    std::size_t index,
    source_root& output) noexcept {

    output = {};

    if (index >= file.roots) {
        return {status_code::artifact_corrupt};
    }

    const auto& roots =
        section(file, section_index::roots);

    const auto* record =
        roots.data + std::uint64_t(index) * 8;

    const auto id_value = read32(record);
    const auto role =
        std::to_integer<unsigned char>(record[4]);

    if (id_value == 0 ||
        id_value > file.sources ||
        role > 1) {
        return {status_code::artifact_corrupt};
    }

    output = {
        source_id{id_value},
        role == 0
            ? project_item_role::type
            : project_item_role::source
    };

    return {};
}

status validate_acyclic_from_roots(
    const decoded_file& file) noexcept {

    std::vector<std::uint8_t> state(file.sources + 1);

    const auto visit =
        [&](auto&& self, source_id id) -> bool {
            auto& current = state[id.value()];

            if (current == 1) {
                return false;
            }

            if (current == 2) {
                return true;
            }

            current = 1;

            std::vector<source_id> dependencies;

            if (!checked_dependencies(
                    file,
                    id,
                    false,
                    dependencies).ok()) {
                return false;
            }

            for (auto dependency : dependencies) {
                if (!self(self, dependency)) {
                    return false;
                }
            }

            current = 2;
            return true;
        };

    for (std::uint32_t index = 0;
         index != file.roots;
         ++index) {
        source_root root;
        const auto result =
            read_root(file, index, root);

        if (!result.ok() ||
            !visit(visit, root.source)) {
            return {status_code::artifact_corrupt};
        }
    }

    return {};
}

struct checkpoint_snapshot {
    std::size_t source_count = 0;
    std::size_t root_count = 0;

    std::vector<std::string> paths;

    std::uint64_t path_bytes = 0;
    std::uint64_t forward_edges = 0;
    std::uint64_t reverse_edges = 0;
};

status collect_checkpoint_snapshot(
    const source_manager& manager,
    checkpoint_snapshot& snapshot) noexcept {

    snapshot.source_count = manager.source_count();
    snapshot.root_count = manager.root_count();

    if (snapshot.source_count >
        std::numeric_limits<std::uint32_t>::max() - 1) {
        return {status_code::persistence_failed};
    }

    snapshot.paths.resize(snapshot.source_count);

    for (std::uint32_t value = 1;
         value <= snapshot.source_count;
         ++value) {
        const auto id = source_id{value};

        std::filesystem::path path;
        auto result = manager.get_path(id, path);

        if (!result.ok()) {
            return result;
        }

        auto& encoded = snapshot.paths[value - 1];

        result = encode_checkpoint_path(path, encoded);

        if (!result.ok()) {
            return result;
        }

        if (encoded.empty()) {
            return {status_code::persistence_failed};
        }

        if (encoded.size() >
            std::numeric_limits<std::uint32_t>::max() -
                snapshot.path_bytes) {
            return {status_code::persistence_failed};
        }

        snapshot.path_bytes += encoded.size();
        snapshot.forward_edges += manager.includes(id).size();
        snapshot.reverse_edges += manager.dependents(id).size();
    }

    if (snapshot.forward_edges >
            std::numeric_limits<std::uint32_t>::max() ||
        snapshot.reverse_edges >
            std::numeric_limits<std::uint32_t>::max() ||
        snapshot.root_count >
            std::numeric_limits<std::uint32_t>::max()) {
        return {status_code::persistence_failed};
    }

    return {};
}

std::uint32_t path_index_capacity(
    std::size_t source_count,
    bool& ok) noexcept {

    if (source_count == 0) {
        return 0;
    }

    const std::uint64_t required =
        (std::uint64_t(source_count) * 10 + 6) / 7;

    std::uint32_t capacity = 1;

    while (capacity < required) {
        if (capacity >
            std::numeric_limits<std::uint32_t>::max() / 2) {
            ok = false;
            return 0;
        }

        capacity *= 2;
    }

    return capacity;
}

struct checkpoint_layout {
    std::array<std::uint32_t, section_count> counts{};
    std::array<std::uint64_t, section_count> offsets{};
    std::array<std::uint64_t, section_count> byte_sizes{};

    std::uint32_t index_capacity = 0;
    std::uint64_t total_size = 0;
};

status build_checkpoint_layout(
    const checkpoint_snapshot& snapshot,
    checkpoint_layout& layout) noexcept {

    bool ok = true;

    layout.index_capacity =
        path_index_capacity(snapshot.source_count, ok);

    if (!ok) {
        return {status_code::persistence_failed};
    }

    layout.counts = {
        static_cast<std::uint32_t>(snapshot.source_count),
        static_cast<std::uint32_t>(snapshot.source_count),
        static_cast<std::uint32_t>(snapshot.source_count + 1),
        static_cast<std::uint32_t>(snapshot.forward_edges),
        static_cast<std::uint32_t>(snapshot.source_count + 1),
        static_cast<std::uint32_t>(snapshot.reverse_edges),
        static_cast<std::uint32_t>(snapshot.root_count),
        layout.index_capacity,
        static_cast<std::uint32_t>(snapshot.path_bytes)
    };

    std::uint64_t cursor =
        header_size + directory_size;

    for (std::size_t index = 0;
         index != section_count;
         ++index) {
        cursor = align_up(
            cursor,
            section_alignments[index],
            ok);

        layout.offsets[index] = cursor;

        layout.byte_sizes[index] =
            std::uint64_t(layout.counts[index]) *
            section_element_sizes[index];

        if (cursor >
            std::numeric_limits<std::uint64_t>::max() -
                layout.byte_sizes[index]) {
            ok = false;
        }

        cursor += layout.byte_sizes[index];
    }

    if (!ok ||
        cursor > std::numeric_limits<std::size_t>::max()) {
        return {status_code::persistence_failed};
    }

    layout.total_size = cursor;
    return {};
}

std::byte* section_data(
    std::vector<std::byte>& bytes,
    const checkpoint_layout& layout,
    section_index index) noexcept {

    return
        bytes.data() +
        layout.offsets[to_index(index)];
}

void write_header_and_directory(
    std::vector<std::byte>& bytes,
    const checkpoint_layout& layout) noexcept {

    std::memcpy(
        bytes.data(),
        magic.data(),
        magic.size());

    write16(bytes.data() + 8, format_major);
    write16(bytes.data() + 10, format_minor);
    write32(bytes.data() + 12, header_size);
    write32(bytes.data() + 16, container_kind);
    write32(bytes.data() + 20, endian_marker);
    write64(bytes.data() + 24, layout.total_size);

    write32(
        bytes.data() + 32,
        layout.counts[to_index(section_index::source_core)]);

    write32(
        bytes.data() + 36,
        layout.counts[to_index(section_index::roots)]);

    write32(
        bytes.data() + 40,
        layout.counts[to_index(section_index::forward_edges)]);

    write32(
        bytes.data() + 44,
        layout.counts[to_index(section_index::reverse_edges)]);

    write32(
        bytes.data() + 48,
        layout.index_capacity);

    write32(
        bytes.data() + 52,
        layout.counts[to_index(section_index::path_bytes)]);

    write32(bytes.data() + 56, section_count);
    write32(bytes.data() + 60, section_entry_size);
    write64(bytes.data() + 64, directory_offset);

    for (std::size_t index = 0;
         index != section_count;
         ++index) {
        auto* entry =
            bytes.data() +
            directory_offset +
            index * section_entry_size;

        write32(
            entry,
            static_cast<std::uint32_t>(index + 1));

        write64(entry + 8, layout.offsets[index]);
        write64(entry + 16, layout.byte_sizes[index]);
        write32(entry + 24, layout.counts[index]);
        write32(entry + 28, section_element_sizes[index]);
    }
}

status write_source_state(
    const source_manager& manager,
    const checkpoint_snapshot& snapshot,
    const checkpoint_layout& layout,
    std::vector<std::byte>& bytes) noexcept {

    auto* source_core =
        section_data(
            bytes,
            layout,
            section_index::source_core);

    auto* physical_state =
        section_data(
            bytes,
            layout,
            section_index::physical_state);

    auto* path_bytes =
        section_data(
            bytes,
            layout,
            section_index::path_bytes);

    std::uint32_t path_offset = 0;

    for (std::uint32_t value = 1;
         value <= snapshot.source_count;
         ++value) {
        const auto id = source_id{value};
        const auto& path = snapshot.paths[value - 1];

        auto* source_record =
            source_core +
            std::uint64_t(value - 1) * 8;

        write32(source_record, path_offset);
        write32(
            source_record + 4,
            static_cast<std::uint32_t>(path.size()));

        std::memcpy(
            path_bytes + path_offset,
            path.data(),
            path.size());

        path_offset +=
            static_cast<std::uint32_t>(path.size());

        source_physical_state physical;
        const auto result =
            manager.get_physical_state(id, physical);

        if (!result.ok() &&
            result.code != status_code::invalid_state) {
            return result;
        }

        if (!result.ok() ||
            physical.presence != source_presence::present) {
            continue;
        }

        auto* record =
            physical_state +
            std::uint64_t(value - 1) * 56;

        write64(
            record,
            physical.observation.write_time_ticks);

        write64(
            record + 8,
            physical.observation.size);

        std::memcpy(
            record + 16,
            physical.hash.bytes.data(),
            physical.hash.bytes.size());

        write32(record + 48, 1);
    }

    return {};
}

void write_dependency_csr(
    const source_manager& manager,
    std::size_t source_count,
    bool reverse,
    const checkpoint_layout& layout,
    std::vector<std::byte>& bytes) noexcept {

    auto* offsets =
        section_data(
            bytes,
            layout,
            reverse
                ? section_index::reverse_offsets
                : section_index::forward_offsets);

    auto* edges =
        section_data(
            bytes,
            layout,
            reverse
                ? section_index::reverse_edges
                : section_index::forward_edges);

    std::uint32_t edge_at = 0;
    write32(offsets, 0);

    for (std::uint32_t value = 1;
         value <= source_count;
         ++value) {
        const auto id = source_id{value};

        if (reverse) {
            for (auto edge : manager.dependents(id)) {
                write32(
                    edges +
                        std::uint64_t(edge_at++) *
                            sizeof(std::uint32_t),
                    edge.value());
            }
        }
        else {
            for (auto edge : manager.includes(id)) {
                write32(
                    edges +
                        std::uint64_t(edge_at++) *
                            sizeof(std::uint32_t),
                    edge.value());
            }
        }

        write32(
            offsets +
                std::uint64_t(value) *
                    sizeof(std::uint32_t),
            edge_at);
    }
}

status write_roots(
    const source_manager& manager,
    const checkpoint_layout& layout,
    std::vector<std::byte>& bytes) noexcept {

    auto* roots =
        section_data(
            bytes,
            layout,
            section_index::roots);

    for (std::size_t index = 0;
         index != manager.root_count();
         ++index) {
        source_root root;
        const auto result =
            manager.get_root(index, root);

        if (!result.ok()) {
            return result;
        }

        auto* record =
            roots + std::uint64_t(index) * 8;

        write32(record, root.source.value());

        record[4] =
            std::byte(
                root.role == project_item_role::type
                    ? 0
                    : 1);
    }

    return {};
}

void write_path_index(
    const checkpoint_snapshot& snapshot,
    const checkpoint_layout& layout,
    std::vector<std::byte>& bytes) noexcept {

    if (snapshot.source_count == 0) {
        return;
    }

    auto* index =
        section_data(
            bytes,
            layout,
            section_index::path_index);

    for (std::uint32_t id = 1;
         id <= snapshot.source_count;
         ++id) {
        const auto& path = snapshot.paths[id - 1];
        const auto hash = source_path_xxh64(path);
        const auto fingerprint =
            source_path_fingerprint(hash);

        auto bucket =
            std::uint32_t(hash) &
            (layout.index_capacity - 1);

        while (read32(
                   index +
                   std::uint64_t(bucket) * 8 +
                   4) != 0) {
            bucket =
                (bucket + 1) &
                (layout.index_capacity - 1);
        }

        auto* record =
            index + std::uint64_t(bucket) * 8;

        write32(record, fingerprint);
        write32(record + 4, id);
    }
}

void seal_checkpoint(std::vector<std::byte>& bytes) noexcept {
    const auto digest =
        core::sha256({
            reinterpret_cast<const char*>(
                bytes.data() + header_size),
            bytes.size() - header_size
        });

    std::memcpy(
        bytes.data() + payload_hash_offset,
        digest.data(),
        digest.size());

    write32(
        bytes.data() + header_crc_offset,
        0);

    const auto header_crc =
        source_manager_crc32c(
            std::span<const std::byte>{
                bytes.data(),
                header_size
            });

    write32(
        bytes.data() + header_crc_offset,
        header_crc);
}

void record_checkpoint_size_metrics(
    metrics_store* metrics,
    const checkpoint_snapshot& snapshot,
    const checkpoint_layout& layout,
    std::size_t artifact_bytes) noexcept {

    if (!metrics) {
        return;
    }

    metrics->increment(
        metric_id::source_checkpoint_source_count,
        snapshot.source_count);

    metrics->increment(
        metric_id::source_checkpoint_root_count,
        snapshot.root_count);

    metrics->increment(
        metric_id::source_checkpoint_forward_edge_count,
        snapshot.forward_edges);

    metrics->increment(
        metric_id::source_checkpoint_reverse_edge_count,
        snapshot.reverse_edges);

    metrics->increment(
        metric_id::source_checkpoint_path_bytes,
        snapshot.path_bytes);

    metrics->increment(
        metric_id::source_checkpoint_path_index_capacity,
        layout.index_capacity);

    metrics->increment(
        metric_id::source_checkpoint_artifact_bytes,
        artifact_bytes);

    metrics->increment(
        metric_id::source_checkpoint_bytes_hashed,
        artifact_bytes - header_size);
}

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
        std::to_string(static_cast<unsigned long long>(::getpid())) +
        "." +
        std::to_string(static_cast<unsigned long long>(stamp));
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

    return output.valid()
               ? status{}
               : status{status_code::persistence_failed};
}

status write_all(
    native_handle file,
    std::span<const std::byte> bytes,
    std::size_t& written) noexcept {

    written = 0;

    while (written < bytes.size()) {
        const auto remaining = bytes.size() - written;

#ifdef _WIN32
        const auto request =
            static_cast<DWORD>(
                (std::min)(
                    remaining,
                    std::size_t(
                        std::numeric_limits<DWORD>::max())));

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
                    std::numeric_limits<ssize_t>::max()));

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
    return FlushFileBuffers(file)
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

    return published
               ? status{}
               : status{status_code::persistence_failed};
#else
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        return {status_code::persistence_failed};
    }

    return sync_parent_directory(target);
#endif
}

} // namespace

std::uint64_t source_path_xxh64(
    std::string_view value) noexcept {

    constexpr std::uint64_t prime1 = 11400714785074694791ull;
    constexpr std::uint64_t prime2 = 14029467366897019727ull;
    constexpr std::uint64_t prime3 = 1609587929392839161ull;
    constexpr std::uint64_t prime4 = 9650029242287828579ull;
    constexpr std::uint64_t prime5 = 2870177450012600261ull;

    const auto* position =
        reinterpret_cast<const std::byte*>(value.data());

    const auto* end =
        position + value.size();

    std::uint64_t hash = 0;

    if (value.size() >= 32) {
        std::uint64_t v1 = prime1 + prime2;
        std::uint64_t v2 = prime2;
        std::uint64_t v3 = 0;
        std::uint64_t v4 = 0 - prime1;

        const auto* limit = end - 32;

        do {
            v1 = xx_round(v1, read64(position));
            position += 8;

            v2 = xx_round(v2, read64(position));
            position += 8;

            v3 = xx_round(v3, read64(position));
            position += 8;

            v4 = xx_round(v4, read64(position));
            position += 8;
        }
        while (position <= limit);

        hash =
            std::rotl(v1, 1) +
            std::rotl(v2, 7) +
            std::rotl(v3, 12) +
            std::rotl(v4, 18);

        const std::array lanes{v1, v2, v3, v4};

        for (auto lane : lanes) {
            hash ^= xx_round(0, lane);
            hash = hash * prime1 + prime4;
        }
    }
    else {
        hash = prime5;
    }

    hash += value.size();

    while (position + 8 <= end) {
        hash ^= xx_round(0, read64(position));
        hash =
            std::rotl(hash, 27) * prime1 +
            prime4;

        position += 8;
    }

    if (position + 4 <= end) {
        hash ^=
            std::uint64_t(read32(position)) *
            prime1;

        hash =
            std::rotl(hash, 23) * prime2 +
            prime3;

        position += 4;
    }

    while (position < end) {
        hash ^=
            std::to_integer<unsigned char>(*position++) *
            prime5;

        hash =
            std::rotl(hash, 11) *
            prime1;
    }

    hash ^= hash >> 33;
    hash *= prime2;
    hash ^= hash >> 29;
    hash *= prime3;
    hash ^= hash >> 32;

    return hash;
}

std::uint32_t source_path_fingerprint(
    std::uint64_t hash) noexcept {

    const auto fingerprint =
        std::uint32_t(hash) ^
        std::uint32_t(hash >> 32);

    return fingerprint != 0
               ? fingerprint
               : 1;
}

std::uint32_t source_manager_crc32c(
    std::span<const std::byte> bytes) noexcept {

    std::uint32_t crc = 0xffffffffu;

    for (auto byte : bytes) {
        crc ^= std::to_integer<unsigned char>(byte);

        for (int bit = 0; bit != 8; ++bit) {
            crc =
                (crc >> 1) ^
                (0x82f63b78u &
                 (0u - (crc & 1u)));
        }
    }

    return crc ^ 0xffffffffu;
}

status encode_wtf8(
    std::wstring_view input,
    std::string& output) noexcept {

    output.clear();

    try {
        output.reserve(input.size() * 3);

        for (std::size_t index = 0;
             index != input.size();
             ++index) {
            std::uint32_t code_point = 0;

            if constexpr (sizeof(wchar_t) == 2) {
                code_point =
                    static_cast<std::uint16_t>(input[index]);

                if (code_point >= 0xd800 &&
                    code_point <= 0xdbff &&
                    index + 1 < input.size()) {
                    const auto low =
                        std::uint32_t(
                            static_cast<std::uint16_t>(
                                input[index + 1]));

                    if (low >= 0xdc00 && low <= 0xdfff) {
                        code_point =
                            0x10000 +
                            ((code_point - 0xd800) << 10) +
                            (low - 0xdc00);
                        ++index;
                    }
                }
            }
            else {
                using unsigned_wchar =
                    std::make_unsigned_t<wchar_t>;

                code_point =
                    static_cast<std::uint32_t>(
                        static_cast<unsigned_wchar>(input[index]));
            }

            if (code_point == 0 || code_point > 0x10ffff) {
                return {status_code::configuration_failed};
            }

            if (code_point < 0x80) {
                output.push_back(char(code_point));
            }
            else if (code_point < 0x800) {
                output.push_back(
                    char(0xc0 | (code_point >> 6)));
                output.push_back(
                    char(0x80 | (code_point & 63)));
            }
            else if (code_point < 0x10000) {
                output.push_back(
                    char(0xe0 | (code_point >> 12)));
                output.push_back(
                    char(0x80 | ((code_point >> 6) & 63)));
                output.push_back(
                    char(0x80 | (code_point & 63)));
            }
            else {
                output.push_back(
                    char(0xf0 | (code_point >> 18)));
                output.push_back(
                    char(0x80 | ((code_point >> 12) & 63)));
                output.push_back(
                    char(0x80 | ((code_point >> 6) & 63)));
                output.push_back(
                    char(0x80 | (code_point & 63)));
            }
        }

        return {};
    }
    catch (...) {
        output.clear();
        return {status_code::initialization_failed};
    }
}

status decode_wtf8(
    std::string_view input,
    std::wstring& output) noexcept {

    output.clear();

    try {
        output.reserve(input.size());

        for (std::size_t index = 0;
             index != input.size();) {
            const auto first =
                static_cast<unsigned char>(input[index++]);

            std::uint32_t code_point = 0;
            unsigned remaining = 0;

            if (first < 0x80) {
                code_point = first;
            }
            else if (first >= 0xc2 && first <= 0xdf) {
                code_point = first & 31;
                remaining = 1;
            }
            else if (first >= 0xe0 && first <= 0xef) {
                code_point = first & 15;
                remaining = 2;
            }
            else if (first >= 0xf0 && first <= 0xf4) {
                code_point = first & 7;
                remaining = 3;
            }
            else {
                return {status_code::artifact_corrupt};
            }

            if (index + remaining > input.size()) {
                return {status_code::artifact_corrupt};
            }

            for (unsigned byte = 0;
                 byte != remaining;
                 ++byte) {
                const auto continuation =
                    static_cast<unsigned char>(input[index++]);

                if ((continuation & 0xc0) != 0x80) {
                    return {status_code::artifact_corrupt};
                }

                code_point =
                    (code_point << 6) |
                    (continuation & 63);
            }

            const bool invalid =
                (remaining == 1 && code_point < 0x80) ||
                (remaining == 2 && code_point < 0x800) ||
                (remaining == 3 && code_point < 0x10000) ||
                code_point > 0x10ffff ||
                code_point == 0;

            if (invalid) {
                return {status_code::artifact_corrupt};
            }

            if constexpr (sizeof(wchar_t) == 2) {
                if (code_point <= 0xffff) {
                    output.push_back(
                        static_cast<wchar_t>(code_point));
                }
                else {
                    code_point -= 0x10000;
                    output.push_back(
                        static_cast<wchar_t>(
                            0xd800 + (code_point >> 10)));
                    output.push_back(
                        static_cast<wchar_t>(
                            0xdc00 + (code_point & 1023)));
                }
            }
            else {
                output.push_back(
                    static_cast<wchar_t>(code_point));
            }
        }

        std::string canonical;
        const auto result = encode_wtf8(output, canonical);

        if (!result.ok() || canonical != input) {
            output.clear();
            return {status_code::artifact_corrupt};
        }

        return {};
    }
    catch (...) {
        output.clear();
        return {status_code::initialization_failed};
    }
}

struct stable_source_manager_view::implementation {
    mapped_file mapping;
    decoded_file file;
};

stable_source_manager_view::stable_source_manager_view() noexcept = default;
stable_source_manager_view::~stable_source_manager_view() = default;

status stable_source_manager_view::open(
    const std::filesystem::path& path) noexcept {

    try {
        auto candidate =
            std::make_unique<implementation>();

        auto result =
            map_readonly(
                path,
                candidate->mapping);

        if (!result.ok()) {
            return result;
        }

        result =
            decode_container(
                static_cast<const std::byte*>(
                    candidate->mapping.view.data),
                candidate->mapping.size,
                candidate->file);

        if (!result.ok()) {
            return result;
        }

        state = std::move(candidate);
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

std::size_t stable_source_manager_view::source_count() const noexcept {
    return state
               ? state->file.sources
               : 0;
}

std::size_t stable_source_manager_view::root_count() const noexcept {
    return state
               ? state->file.roots
               : 0;
}

status stable_source_manager_view::path(
    source_id id,
    std::filesystem::path& output) const noexcept {

    output.clear();

    if (!state) {
        return {status_code::invalid_state};
    }

    std::string_view bytes;
    auto result =
        checked_path_bytes(
            state->file,
            id,
            bytes);

    if (!result.ok()) {
        return result;
    }

    return decode_checkpoint_path(bytes, output);
}

status stable_source_manager_view::find(
    const std::filesystem::path& path_value,
    source_id& output) const noexcept {

    output = {};

    if (!state) {
        return {status_code::invalid_state};
    }

    std::string encoded;
    auto result =
        encode_checkpoint_path(
            path_value,
            encoded);

    if (!result.ok()) {
        return result;
    }

    const auto& file = state->file;

    if (file.index_capacity == 0) {
        return {status_code::not_available};
    }

    const auto hash =
        source_path_xxh64(encoded);

    const auto fingerprint =
        source_path_fingerprint(hash);

    const auto& index =
        section(file, section_index::path_index);

    auto bucket =
        std::uint32_t(hash) &
        (file.index_capacity - 1);

    for (std::uint32_t probes = 0;
         probes != file.index_capacity;
         ++probes,
         bucket = (bucket + 1) &
                  (file.index_capacity - 1)) {
        const auto* record =
            index.data +
            std::uint64_t(bucket) * 8;

        const auto stored_fingerprint =
            read32(record);

        const auto id_value =
            read32(record + 4);

        if (id_value == 0) {
            if (stored_fingerprint != 0) {
                return {status_code::artifact_corrupt};
            }

            return {status_code::not_available};
        }

        if (id_value > file.sources) {
            return {status_code::artifact_corrupt};
        }

        if (stored_fingerprint != fingerprint) {
            continue;
        }

        std::string_view stored;
        result =
            checked_path_bytes(
                file,
                source_id{id_value},
                stored);

        if (!result.ok()) {
            return result;
        }

        if (stored == encoded) {
            output = source_id{id_value};
            return {};
        }
    }

    return {status_code::artifact_corrupt};
}

status stable_source_manager_view::physical(
    source_id id,
    source_physical_state& output) const noexcept {

    output = {};

    if (!state ||
        !id ||
        id.value() > state->file.sources) {
        return {status_code::artifact_corrupt};
    }

    const auto& physical =
        section(
            state->file,
            section_index::physical_state);

    const auto* record =
        physical.data +
        std::uint64_t(id.value() - 1) * 56;

    const auto flags =
        read32(record + 48);

    if ((flags & 1u) != 0) {
        output.presence =
            source_presence::present;

        output.observation.write_time_ticks =
            read64(record);

        output.observation.size =
            read64(record + 8);

        std::memcpy(
            output.hash.bytes.data(),
            record + 16,
            output.hash.bytes.size());
    }

    return {};
}

status stable_source_manager_view::root(
    std::size_t index,
    source_root& output) const noexcept {

    if (!state) {
        output = {};
        return {status_code::artifact_corrupt};
    }

    return read_root(
        state->file,
        index,
        output);
}

status stable_source_manager_view::dependencies(
    source_id id,
    bool reverse,
    std::vector<source_id>& output) const noexcept {

    if (!state) {
        return {status_code::invalid_state};
    }

    return checked_dependencies(
        state->file,
        id,
        reverse,
        output);
}

status stable_source_manager_view::validate_artifact() const noexcept {
    if (!state) {
        return {status_code::invalid_state};
    }

    const auto& file = state->file;

    auto result = validate_payload_hash(file);

    if (!result.ok()) {
        return result;
    }

    result = validate_paths_and_physical(file);

    if (!result.ok()) {
        return result;
    }

    result = validate_csr(file, false);

    if (!result.ok()) {
        return result;
    }

    result = validate_csr(file, true);

    if (!result.ok()) {
        return result;
    }

    result = validate_roots(file);

    if (!result.ok()) {
        return result;
    }

    return validate_path_index(file);
}

status stable_source_manager_view::strict_validate() const noexcept {
    const auto artifact_result =
        validate_artifact();

    if (!artifact_result.ok()) {
        return artifact_result;
    }

    auto result =
        validate_dependency_symmetry(
            state->file);

    if (!result.ok()) {
        return result;
    }

    return validate_acyclic_from_roots(
        state->file);
}

status strict_validate_source_manager_checkpoint(
    const std::filesystem::path& path) noexcept {

    stable_source_manager_view view;
    const auto result = view.open(path);

    return result.ok()
               ? view.strict_validate()
               : result;
}

status write_source_manager_checkpoint(
    const source_manager& manager,
    const std::filesystem::path& target,
    metrics_store* metrics) noexcept {

    save_metric_scope save_metrics{metrics};

    try {
        checkpoint_snapshot snapshot;

        auto phase_begin =
            persistence_clock::now();

        auto result =
            collect_checkpoint_snapshot(
                manager,
                snapshot);

        if (!result.ok()) {
            return result;
        }

        record_phase(
            metrics,
            metric_id::source_checkpoint_path_materialization_duration,
            phase_begin);

        phase_begin = persistence_clock::now();

        checkpoint_layout layout;
        result =
            build_checkpoint_layout(
                snapshot,
                layout);

        if (!result.ok()) {
            return result;
        }

        std::vector<std::byte> bytes(
            static_cast<std::size_t>(
                layout.total_size));

        write_header_and_directory(
            bytes,
            layout);

        record_phase(
            metrics,
            metric_id::source_checkpoint_snapshot_layout_duration,
            phase_begin);

        phase_begin = persistence_clock::now();

        result =
            write_source_state(
                manager,
                snapshot,
                layout,
                bytes);

        if (!result.ok()) {
            return result;
        }

        record_phase(
            metrics,
            metric_id::source_checkpoint_source_state_duration,
            phase_begin);

        phase_begin = persistence_clock::now();

        write_dependency_csr(
            manager,
            snapshot.source_count,
            false,
            layout,
            bytes);

        record_phase(
            metrics,
            metric_id::source_checkpoint_forward_csr_duration,
            phase_begin);

        phase_begin = persistence_clock::now();

        write_dependency_csr(
            manager,
            snapshot.source_count,
            true,
            layout,
            bytes);

        record_phase(
            metrics,
            metric_id::source_checkpoint_reverse_csr_duration,
            phase_begin);

        result =
            write_roots(
                manager,
                layout,
                bytes);

        if (!result.ok()) {
            return result;
        }

        phase_begin = persistence_clock::now();

        write_path_index(
            snapshot,
            layout,
            bytes);

        record_phase(
            metrics,
            metric_id::source_checkpoint_path_index_duration,
            phase_begin);

        phase_begin = persistence_clock::now();

        seal_checkpoint(bytes);

        record_phase(
            metrics,
            metric_id::source_checkpoint_payload_sha256_duration,
            phase_begin);

        record_checkpoint_size_metrics(
            metrics,
            snapshot,
            layout,
            bytes.size());

        const auto temporary =
            temporary_checkpoint_path(target);

        unique_handle file;
        result = create_new_write_file(temporary, file);

        if (!result.ok()) {
            return result;
        }

        phase_begin = persistence_clock::now();

        std::size_t written = 0;
        result =
            write_all(
                file.value,
                bytes,
                written);

        if (!result.ok()) {
            file.close();
            delete_file(temporary);
            return result;
        }

        record_phase(
            metrics,
            metric_id::source_checkpoint_write_duration,
            phase_begin);

        if (metrics) {
            metrics->increment(
                metric_id::source_checkpoint_bytes_written,
                written);
        }

        phase_begin = persistence_clock::now();

        result = flush_file(file.value);

        if (!result.ok()) {
            file.close();
            delete_file(temporary);
            return result;
        }

        record_phase(
            metrics,
            metric_id::source_checkpoint_flush_duration,
            phase_begin);

        file.close();

        status validation;

        {
            stable_source_manager_view validation_view;

            phase_begin =
                persistence_clock::now();

            validation =
                validation_view.open(temporary);

            record_phase(
                metrics,
                metric_id::source_checkpoint_reopen_map_duration,
                phase_begin);
        }

        if (!validation.ok()) {
            delete_file(temporary);
            return validation;
        }

        result =
            publish_checkpoint(
                temporary,
                target);

        if (!result.ok()) {
            delete_file(temporary);
            return result;
        }

        save_metrics.success = true;
        return {};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

} // namespace cw::server
