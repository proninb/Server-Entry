#include "source_manager_persistence.hpp"

#include "../../core/hash/sha256.hpp"
#include "../../metrics/metrics_store.hpp"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

namespace cw::server
{
namespace
{
using persistence_clock = std::chrono::steady_clock;

struct save_metric_scope
{
    metrics_store* store;
    persistence_clock::time_point start = persistence_clock::now();
    bool success = false;

    explicit save_metric_scope(metrics_store* value) noexcept : store(value)
    {
        if (store) store->increment(metric_id::source_checkpoint_save_attempt_count);
    }
    ~save_metric_scope()
    {
        if (!store) return;
        store->record_duration(metric_id::source_checkpoint_save_duration,
            std::chrono::duration_cast<std::chrono::nanoseconds>(persistence_clock::now()-start));
        store->increment(success ? metric_id::source_checkpoint_save_success_count
                                 : metric_id::source_checkpoint_save_failure_count);
    }
};

void record_phase(metrics_store* store, metric_id id,
                  persistence_clock::time_point begin) noexcept
{
    if (store) store->record_duration(id,
        std::chrono::duration_cast<std::chrono::nanoseconds>(persistence_clock::now()-begin));
}

constexpr std::size_t header_size = 128;
constexpr std::size_t directory_offset = 128;
constexpr std::size_t section_count = 9;
constexpr std::size_t section_entry_size = 32;
constexpr std::size_t directory_size = section_count * section_entry_size;
constexpr std::array<char, 8> magic{'C','W','S','M','B','I','N','1'};

enum section_kind : std::uint32_t
{
    source_core_section = 1, physical_state_section, forward_offsets_section,
    forward_edges_section, reverse_offsets_section, reverse_edges_section,
    roots_section, path_index_section, path_bytes_section
};

struct section_view { const std::byte* data{}; std::uint64_t size{}; std::uint32_t count{}; std::uint32_t element_size{}; };

std::uint16_t read16(const std::byte* p) noexcept
{ return std::uint16_t(std::to_integer<unsigned char>(p[0])) | std::uint16_t(std::to_integer<unsigned char>(p[1])) << 8; }
std::uint32_t read32(const std::byte* p) noexcept
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) value |= std::uint32_t(std::to_integer<unsigned char>(p[i])) << (i * 8);
    return value;
}
std::uint64_t read64(const std::byte* p) noexcept
{
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) value |= std::uint64_t(std::to_integer<unsigned char>(p[i])) << (i * 8);
    return value;
}
void write16(std::byte* p, std::uint16_t value) noexcept
{ for (unsigned i = 0; i != 2; ++i) p[i] = std::byte((value >> (i * 8)) & 0xff); }
void write32(std::byte* p, std::uint32_t value) noexcept
{ for (unsigned i = 0; i != 4; ++i) p[i] = std::byte((value >> (i * 8)) & 0xff); }
void write64(std::byte* p, std::uint64_t value) noexcept
{ for (unsigned i = 0; i != 8; ++i) p[i] = std::byte((value >> (i * 8)) & 0xff); }

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment, bool& ok) noexcept
{
    const auto mask = alignment - 1;
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) { ok = false; return 0; }
    return (value + mask) & ~mask;
}

struct unique_handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~unique_handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
    void close() noexcept
    {
        if(value!=INVALID_HANDLE_VALUE&&value!=nullptr)CloseHandle(value);
        value=INVALID_HANDLE_VALUE;
    }
};

struct unique_mapping
{
    void* value = nullptr;
    ~unique_mapping() { if (value) UnmapViewOfFile(value); }
};

struct mapped_file
{
    unique_handle file;
    unique_handle mapping;
    unique_mapping view;
    std::uint64_t size = 0;
};

status map_readonly(const std::filesystem::path& path, mapped_file& output) noexcept
{
    output.file.value = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output.file.value == INVALID_HANDLE_VALUE) return {status_code::persistence_failed};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(output.file.value, &size) || size.QuadPart < 0)
        return {status_code::persistence_failed};
    output.size = static_cast<std::uint64_t>(size.QuadPart);
    output.mapping.value = CreateFileMappingW(output.file.value, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!output.mapping.value) return {status_code::persistence_failed};
    output.view.value = MapViewOfFile(output.mapping.value, FILE_MAP_READ, 0, 0, 0);
    return output.view.value ? status{} : status{status_code::persistence_failed};
}

std::uint64_t xx_round(std::uint64_t accumulator, std::uint64_t input) noexcept
{
    accumulator += input * 14029467366897019727ull;
    accumulator = std::rotl(accumulator, 31);
    return accumulator * 11400714785074694791ull;
}

struct decoded_file
{
    const std::byte* base{};
    std::uint64_t file_size{};
    std::uint32_t sources{}, roots{}, forward_edges{}, reverse_edges{}, index_capacity{}, path_bytes{};
    std::array<section_view, section_count> sections{};
};

status decode_container(const std::byte* base, std::uint64_t actual_size,
                        decoded_file& output) noexcept
{
    if (!base || actual_size < header_size + directory_size) return {status_code::artifact_corrupt};
    if (std::memcmp(base, magic.data(), magic.size()) != 0 || read16(base + 8) != 1 ||
        read16(base + 10) != 0 || read32(base + 12) != header_size ||
        read32(base + 16) != 1 || read32(base + 20) != 0x01020304 ||
        read64(base + 24) != actual_size || read32(base + 56) != section_count ||
        read32(base + 60) != section_entry_size || read64(base + 64) != directory_offset)
        return {status_code::artifact_corrupt};
    std::array<std::byte, header_size> header{};
    std::memcpy(header.data(), base, header.size());
    const auto stored_crc = read32(header.data() + 104);
    std::fill(header.begin() + 104, header.begin() + 108, std::byte{});
    if (source_manager_crc32c(header) != stored_crc) return {status_code::artifact_corrupt};
    for (std::size_t i = 108; i != 128; ++i) if (base[i] != std::byte{}) return {status_code::artifact_corrupt};

    output.base = base; output.file_size = actual_size;
    output.sources = read32(base + 32); output.roots = read32(base + 36);
    output.forward_edges = read32(base + 40); output.reverse_edges = read32(base + 44);
    output.index_capacity = read32(base + 48); output.path_bytes = read32(base + 52);
    if (output.sources == std::numeric_limits<std::uint32_t>::max()) return {status_code::artifact_corrupt};
    const std::uint32_t expected_sizes[] = {8,56,4,4,4,4,8,8,1};
    const std::uint32_t expected_counts[] = {output.sources,output.sources,output.sources+1,
        output.forward_edges,output.sources+1,output.reverse_edges,output.roots,
        output.index_capacity,output.path_bytes};
    const std::uint32_t alignments[] = {4,8,4,4,4,4,4,4,1};
    std::uint64_t previous_end = header_size + directory_size;
    for (std::size_t i = 0; i != section_count; ++i)
    {
        const auto* entry = base + directory_offset + i * section_entry_size;
        const auto kind = read32(entry); const auto flags = read32(entry + 4);
        const auto offset = read64(entry + 8); const auto byte_size = read64(entry + 16);
        const auto count = read32(entry + 24); const auto element_size = read32(entry + 28);
        if (kind != i + 1 || flags != 0 || element_size != expected_sizes[i] ||
            count != expected_counts[i] || byte_size != std::uint64_t(count) * element_size ||
            offset % alignments[i] != 0 || offset < previous_end || offset > actual_size ||
            byte_size > actual_size - offset) return {status_code::artifact_corrupt};
        for (auto p = previous_end; p != offset; ++p) if (base[p] != std::byte{}) return {status_code::artifact_corrupt};
        output.sections[i] = {base + offset, byte_size, count, element_size};
        previous_end = offset + byte_size;
    }
    if (previous_end != actual_size) return {status_code::artifact_corrupt};
    if (output.sources == 0)
    {
        if (output.roots || output.forward_edges || output.reverse_edges ||
            output.index_capacity || output.path_bytes) return {status_code::artifact_corrupt};
    }
    else if (output.index_capacity == 0 || !std::has_single_bit(output.index_capacity) ||
             std::uint64_t(output.sources) * 10 > std::uint64_t(output.index_capacity) * 7)
        return {status_code::artifact_corrupt};
    return {};
}

status checked_path_bytes(const decoded_file& file, source_id id, std::string_view& output) noexcept
{
    output = {};
    if (!id || id.value() > file.sources) return {status_code::artifact_corrupt};
    const auto* record = file.sections[0].data + std::uint64_t(id.value() - 1) * 8;
    const auto offset = read32(record); const auto length = read32(record + 4);
    if (offset > file.path_bytes || length > file.path_bytes - offset)
        return {status_code::artifact_corrupt};
    output = {reinterpret_cast<const char*>(file.sections[8].data + offset), length};
    return {};
}

status checked_dependencies(const decoded_file& file, source_id id, bool reverse,
                            std::vector<source_id>& output) noexcept
{
    output.clear();
    if (!id || id.value() > file.sources) return {status_code::artifact_corrupt};
    const auto& offsets = file.sections[reverse ? 4 : 2];
    const auto& edges = file.sections[reverse ? 5 : 3];
    const auto begin = read32(offsets.data + std::uint64_t(id.value() - 1) * 4);
    const auto end = read32(offsets.data + std::uint64_t(id.value()) * 4);
    if (begin > end || end > edges.count) return {status_code::artifact_corrupt};
    try { output.reserve(end - begin); }
    catch (...) { return {status_code::initialization_failed}; }
    for (auto i = begin; i != end; ++i)
    {
        const auto value = read32(edges.data + std::uint64_t(i) * 4);
        if (value == 0 || value > file.sources) { output.clear(); return {status_code::artifact_corrupt}; }
        output.push_back(source_id{value});
    }
    return {};
}
}

std::uint64_t source_path_xxh64(std::string_view value) noexcept
{
    constexpr std::uint64_t p1=11400714785074694791ull,p2=14029467366897019727ull,
        p3=1609587929392839161ull,p4=9650029242287828579ull,p5=2870177450012600261ull;
    const auto* position = reinterpret_cast<const std::byte*>(value.data());
    const auto* end = position + value.size(); std::uint64_t hash;
    if (value.size() >= 32)
    {
        std::uint64_t v1=p1+p2,v2=p2,v3=0,v4=0-p1; const auto* limit=end-32;
        do { v1=xx_round(v1,read64(position)); position+=8; v2=xx_round(v2,read64(position)); position+=8;
             v3=xx_round(v3,read64(position)); position+=8; v4=xx_round(v4,read64(position)); position+=8; } while(position<=limit);
        hash=std::rotl(v1,1)+std::rotl(v2,7)+std::rotl(v3,12)+std::rotl(v4,18);
        for(auto v:{v1,v2,v3,v4}) { hash^=xx_round(0,v); hash=hash*p1+p4; }
    } else hash=p5;
    hash+=value.size();
    while(position+8<=end) { hash^=xx_round(0,read64(position)); hash=std::rotl(hash,27)*p1+p4; position+=8; }
    if(position+4<=end) { hash^=std::uint64_t(read32(position))*p1; hash=std::rotl(hash,23)*p2+p3; position+=4; }
    while(position<end) { hash^=std::to_integer<unsigned char>(*position++)*p5; hash=std::rotl(hash,11)*p1; }
    hash^=hash>>33; hash*=p2; hash^=hash>>29; hash*=p3; return hash^(hash>>32);
}

std::uint32_t source_path_fingerprint(std::uint64_t hash) noexcept
{ const auto value=std::uint32_t(hash)^std::uint32_t(hash>>32); return value ? value : 1; }

std::uint32_t source_manager_crc32c(std::span<const std::byte> bytes) noexcept
{
    std::uint32_t crc=0xffffffffu;
    for(auto byte:bytes) { crc^=std::to_integer<unsigned char>(byte); for(int i=0;i!=8;++i) crc=(crc>>1)^(0x82f63b78u & (0u-(crc&1u))); }
    return crc^0xffffffffu;
}

status encode_wtf8(std::wstring_view input, std::string& output) noexcept
{
    output.clear();
    try
    {
        output.reserve(input.size()*3);
        for(std::size_t i=0;i<input.size();++i)
        {
            std::uint32_t cp=static_cast<std::uint16_t>(input[i]); if(cp==0) return {status_code::configuration_failed};
            if(cp>=0xd800 && cp<=0xdbff && i+1<input.size())
            { const auto low=std::uint32_t(static_cast<std::uint16_t>(input[i+1])); if(low>=0xdc00&&low<=0xdfff) { cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00); ++i; } }
            if(cp<0x80) output.push_back(char(cp));
            else if(cp<0x800) { output.push_back(char(0xc0|(cp>>6))); output.push_back(char(0x80|(cp&63))); }
            else if(cp<0x10000) { output.push_back(char(0xe0|(cp>>12))); output.push_back(char(0x80|((cp>>6)&63))); output.push_back(char(0x80|(cp&63))); }
            else { output.push_back(char(0xf0|(cp>>18))); output.push_back(char(0x80|((cp>>12)&63))); output.push_back(char(0x80|((cp>>6)&63))); output.push_back(char(0x80|(cp&63))); }
        }
        return {};
    } catch(...) { output.clear(); return {status_code::initialization_failed}; }
}

status decode_wtf8(std::string_view input, std::wstring& output) noexcept
{
    output.clear();
    try
    {
        output.reserve(input.size());
        for(std::size_t i=0;i<input.size();)
        {
            const auto b0=static_cast<unsigned char>(input[i++]); std::uint32_t cp; unsigned remaining;
            if(b0<0x80) { cp=b0; remaining=0; }
            else if(b0>=0xc2&&b0<=0xdf) { cp=b0&31; remaining=1; }
            else if(b0>=0xe0&&b0<=0xef) { cp=b0&15; remaining=2; }
            else if(b0>=0xf0&&b0<=0xf4) { cp=b0&7; remaining=3; }
            else return {status_code::artifact_corrupt};
            if(i+remaining>input.size()) return {status_code::artifact_corrupt};
            for(unsigned n=0;n<remaining;++n) { const auto b=static_cast<unsigned char>(input[i++]); if((b&0xc0)!=0x80) return {status_code::artifact_corrupt}; cp=(cp<<6)|(b&63); }
            if((remaining==1&&cp<0x80)||(remaining==2&&cp<0x800)||(remaining==3&&cp<0x10000)||cp>0x10ffff||cp==0) return {status_code::artifact_corrupt};
            if(cp<=0xffff) output.push_back(static_cast<wchar_t>(cp));
            else { cp-=0x10000; output.push_back(static_cast<wchar_t>(0xd800+(cp>>10))); output.push_back(static_cast<wchar_t>(0xdc00+(cp&1023))); }
        }
        std::string canonical; const auto result=encode_wtf8(output,canonical);
        if(!result.ok()||canonical!=input) { output.clear(); return {status_code::artifact_corrupt}; }
        return {};
    } catch(...) { output.clear(); return {status_code::initialization_failed}; }
}

struct stable_source_manager_view::implementation { mapped_file mapping; decoded_file file; };
stable_source_manager_view::stable_source_manager_view() noexcept = default;
stable_source_manager_view::~stable_source_manager_view() = default;

status stable_source_manager_view::open(const std::filesystem::path& path) noexcept
{
    try { auto candidate=std::make_unique<implementation>(); auto result=map_readonly(path,candidate->mapping); if(!result.ok()) return result;
        result=decode_container(static_cast<const std::byte*>(candidate->mapping.view.value),candidate->mapping.size,candidate->file);
        if(!result.ok()) return result; implementation_=std::move(candidate); return {}; }
    catch(...) { return {status_code::initialization_failed}; }
}
std::size_t stable_source_manager_view::source_count() const noexcept { return implementation_?implementation_->file.sources:0; }
std::size_t stable_source_manager_view::root_count() const noexcept { return implementation_?implementation_->file.roots:0; }
status stable_source_manager_view::path(source_id id,std::filesystem::path& output) const noexcept
{ output.clear(); if(!implementation_) return {status_code::invalid_state}; std::string_view bytes; auto r=checked_path_bytes(implementation_->file,id,bytes); if(!r.ok())return r; std::wstring decoded; r=decode_wtf8(bytes,decoded); if(!r.ok())return r; try{output=std::filesystem::path{decoded};return{};}catch(...){return{status_code::initialization_failed};} }
status stable_source_manager_view::find(const std::filesystem::path& path_value,source_id& output) const noexcept
{
    output={}; if(!implementation_)return{status_code::invalid_state}; std::string encoded; auto r=encode_wtf8(path_value.native(),encoded); if(!r.ok())return r;
    const auto& f=implementation_->file; if(!f.index_capacity)return {status_code::not_available}; const auto hash=source_path_xxh64(encoded); const auto fp=source_path_fingerprint(hash); auto at=std::uint32_t(hash)&(f.index_capacity-1);
    for(std::uint32_t probes=0;probes<f.index_capacity;++probes,at=(at+1)&(f.index_capacity-1)) { const auto* b=f.sections[7].data+std::uint64_t(at)*8; const auto bfp=read32(b),id=read32(b+4); if(id==0){if(bfp!=0)return{status_code::artifact_corrupt};return{status_code::not_available};} if(id>f.sources)return{status_code::artifact_corrupt}; if(bfp==fp){std::string_view stored;r=checked_path_bytes(f,source_id{id},stored);if(!r.ok())return r;if(stored==encoded){output=source_id{id};return{};}} } return{status_code::artifact_corrupt};
}
status stable_source_manager_view::physical(source_id id,source_physical_state& output) const noexcept
{
    output={}; if(!implementation_||!id||id.value()>implementation_->file.sources)return{status_code::artifact_corrupt}; const auto* p=implementation_->file.sections[1].data+std::uint64_t(id.value()-1)*56; const auto flags=read32(p+48); if(flags&1){output.presence=source_presence::present;output.observation.write_time_ticks=read64(p);output.observation.size=read64(p+8);std::memcpy(output.hash.bytes.data(),p+16,32);}return{};
}
status stable_source_manager_view::root(std::size_t index,source_root& output) const noexcept
{ output={};if(!implementation_||index>=implementation_->file.roots)return{status_code::artifact_corrupt};const auto*p=implementation_->file.sections[6].data+index*8;const auto id=read32(p);const auto role=std::to_integer<unsigned char>(p[4]);if(!id||id>implementation_->file.sources||role>1)return{status_code::artifact_corrupt};output={source_id{id},role==0?project_item_role::type:project_item_role::source};return{}; }
status stable_source_manager_view::dependencies(source_id id,bool reverse,std::vector<source_id>& output) const noexcept
{ if(!implementation_)return{status_code::invalid_state};return checked_dependencies(implementation_->file,id,reverse,output); }

status stable_source_manager_view::validate_artifact() const noexcept
{
    if(!implementation_)return{status_code::invalid_state}; const auto& f=implementation_->file;
    const auto digest=core::sha256({reinterpret_cast<const char*>(f.base+128),static_cast<std::size_t>(f.file_size-128)}); if(std::memcmp(digest.data(),f.base+72,32)!=0)return{status_code::artifact_corrupt};
    std::unordered_set<std::string_view> paths; std::uint32_t expected_offset=0; std::vector<std::uint32_t> seen(f.sources+1);
    try { paths.reserve(f.sources); } catch(...) { return{status_code::initialization_failed}; }
    for(std::uint32_t id=1;id<=f.sources;++id){std::string_view bytes;auto r=checked_path_bytes(f,source_id{id},bytes);if(!r.ok())return r;const auto*rec=f.sections[0].data+std::uint64_t(id-1)*8;if(read32(rec)!=expected_offset||bytes.empty())return{status_code::artifact_corrupt};if(bytes.size()>std::numeric_limits<std::uint32_t>::max()-expected_offset)return{status_code::artifact_corrupt};expected_offset+=static_cast<std::uint32_t>(bytes.size());std::wstring decoded;r=decode_wtf8(bytes,decoded);if(!r.ok()||!paths.insert(bytes).second)return{status_code::artifact_corrupt};const auto*p=f.sections[1].data+std::uint64_t(id-1)*56;const auto flags=read32(p+48);if(flags&~1u||read32(p+52))return{status_code::artifact_corrupt};if(!(flags&1)){for(std::size_t i=0;i<48;++i)if(p[i]!=std::byte{})return{status_code::artifact_corrupt};}}
    if(expected_offset!=f.path_bytes)return{status_code::artifact_corrupt};
    for(bool reverse:{false,true}){const auto&o=f.sections[reverse?4:2];const auto&e=f.sections[reverse?5:3];if(read32(o.data)!=0||read32(o.data+std::uint64_t(f.sources)*4)!=e.count)return{status_code::artifact_corrupt};for(std::uint32_t id=1;id<=f.sources;++id){const auto begin=read32(o.data+std::uint64_t(id-1)*4),end=read32(o.data+std::uint64_t(id)*4);if(begin>end||end>e.count)return{status_code::artifact_corrupt};for(auto i=begin;i<end;++i){const auto v=read32(e.data+std::uint64_t(i)*4);if(!v||v>f.sources)return{status_code::artifact_corrupt};}}}
    for(std::uint32_t i=0;i<f.roots;++i){source_root value;auto r=root(i,value);if(!r.ok())return r;const auto*p=f.sections[6].data+std::uint64_t(i)*8;if(p[5]!=std::byte{}||p[6]!=std::byte{}||p[7]!=std::byte{})return{status_code::artifact_corrupt};}
    for(std::uint32_t i=0;i<f.index_capacity;++i){const auto*b=f.sections[7].data+std::uint64_t(i)*8;const auto fp=read32(b),id=read32(b+4);if(!id){if(fp)return{status_code::artifact_corrupt};continue;}if(id>f.sources||!fp||seen[id]++)return{status_code::artifact_corrupt};std::string_view bytes;auto r=checked_path_bytes(f,source_id{id},bytes);if(!r.ok()||source_path_fingerprint(source_path_xxh64(bytes))!=fp)return{status_code::artifact_corrupt};auto at=std::uint32_t(source_path_xxh64(bytes))&(f.index_capacity-1);bool reachable=false;for(std::uint32_t n=0;n<f.index_capacity;++n,at=(at+1)&(f.index_capacity-1)){const auto*probe=f.sections[7].data+std::uint64_t(at)*8;if(read32(probe+4)==0)break;if(at==i){reachable=true;break;}}if(!reachable)return{status_code::artifact_corrupt};}
    for(std::uint32_t id=1;id<=f.sources;++id)if(seen[id]!=1)return{status_code::artifact_corrupt};
    return{};
}

status stable_source_manager_view::strict_validate() const noexcept
{
    const auto artifact=validate_artifact();if(!artifact.ok())return artifact;
    const auto& f=implementation_->file;
    if(f.forward_edges!=f.reverse_edges)return{status_code::artifact_corrupt};std::unordered_set<std::uint64_t> forward;try{forward.reserve(f.forward_edges);}catch(...){return{status_code::initialization_failed};}for(std::uint32_t parent=1;parent<=f.sources;++parent){std::vector<source_id> values;auto r=checked_dependencies(f,source_id{parent},false,values);if(!r.ok())return r;for(auto child:values)if(!forward.insert((std::uint64_t(parent)<<32)|child.value()).second)return{status_code::artifact_corrupt};}for(std::uint32_t child=1;child<=f.sources;++child){std::vector<source_id> values;auto r=checked_dependencies(f,source_id{child},true,values);if(!r.ok())return r;for(auto parent:values)if(!forward.erase((std::uint64_t(parent.value())<<32)|child))return{status_code::artifact_corrupt};}if(!forward.empty())return{status_code::artifact_corrupt};
    std::vector<std::uint8_t> state(f.sources+1);const auto visit=[&](auto&& self,source_id id)->bool{auto&s=state[id.value()];if(s==1)return false;if(s==2)return true;s=1;std::vector<source_id> values;if(!checked_dependencies(f,id,false,values).ok())return false;for(auto v:values)if(!self(self,v))return false;s=2;return true;};for(std::uint32_t i=0;i<f.roots;++i){source_root value;if(!root(i,value).ok()||!visit(visit,value.source))return{status_code::artifact_corrupt};}
    return{};
}

status strict_validate_source_manager_checkpoint(const std::filesystem::path& path) noexcept
{ stable_source_manager_view view;auto r=view.open(path);return r.ok()?view.strict_validate():r; }

status write_source_manager_checkpoint(const source_manager& manager,const std::filesystem::path& target,metrics_store* metrics) noexcept
{
    save_metric_scope save_metrics{metrics};
    try
    {
        const auto count=manager.source_count();if(count>std::numeric_limits<std::uint32_t>::max()-1)return{status_code::persistence_failed};
        auto phase_begin=persistence_clock::now();
        std::vector<std::string> paths(count);std::uint64_t path_size=0,forward_count=0,reverse_count=0;
        for(std::uint32_t id=1;id<=count;++id){std::filesystem::path path;auto r=manager.get_path(source_id{id},path);if(!r.ok())return r;r=encode_wtf8(path.native(),paths[id-1]);if(!r.ok())return r;if(paths[id-1].empty()||path_size>std::numeric_limits<std::uint32_t>::max()-paths[id-1].size())return{status_code::persistence_failed};path_size+=paths[id-1].size();forward_count+=manager.includes(source_id{id}).size();reverse_count+=manager.dependents(source_id{id}).size();}
        record_phase(metrics,metric_id::source_checkpoint_path_materialization_duration,phase_begin);
        if(forward_count>std::numeric_limits<std::uint32_t>::max()||reverse_count>std::numeric_limits<std::uint32_t>::max()||manager.root_count()>std::numeric_limits<std::uint32_t>::max())return{status_code::persistence_failed};
        phase_begin=persistence_clock::now();std::uint32_t capacity=0;if(count){std::uint64_t need=(count*10+6)/7;capacity=1;while(capacity<need){if(capacity>std::numeric_limits<std::uint32_t>::max()/2)return{status_code::persistence_failed};capacity*=2;}}
        const std::uint32_t counts[]={static_cast<std::uint32_t>(count),static_cast<std::uint32_t>(count),static_cast<std::uint32_t>(count+1),static_cast<std::uint32_t>(forward_count),static_cast<std::uint32_t>(count+1),static_cast<std::uint32_t>(reverse_count),static_cast<std::uint32_t>(manager.root_count()),capacity,static_cast<std::uint32_t>(path_size)};
        const std::uint32_t sizes[]={8,56,4,4,4,4,8,8,1},alignments[]={4,8,4,4,4,4,4,4,1};std::uint64_t cursor=header_size+directory_size;bool ok=true;std::array<std::uint64_t,9> offsets{},byte_sizes{};for(std::size_t i=0;i<9;++i){cursor=align_up(cursor,alignments[i],ok);offsets[i]=cursor;byte_sizes[i]=std::uint64_t(counts[i])*sizes[i];if(cursor>std::numeric_limits<std::uint64_t>::max()-byte_sizes[i])ok=false;cursor+=byte_sizes[i];}if(!ok||cursor>std::numeric_limits<std::size_t>::max())return{status_code::persistence_failed};std::vector<std::byte> bytes(static_cast<std::size_t>(cursor));
        std::memcpy(bytes.data(),magic.data(),8);write16(bytes.data()+8,1);write16(bytes.data()+10,0);write32(bytes.data()+12,128);write32(bytes.data()+16,1);write32(bytes.data()+20,0x01020304);write64(bytes.data()+24,cursor);write32(bytes.data()+32,counts[0]);write32(bytes.data()+36,counts[6]);write32(bytes.data()+40,counts[3]);write32(bytes.data()+44,counts[5]);write32(bytes.data()+48,capacity);write32(bytes.data()+52,counts[8]);write32(bytes.data()+56,9);write32(bytes.data()+60,32);write64(bytes.data()+64,128);
        for(std::size_t i=0;i<9;++i){auto*e=bytes.data()+128+i*32;write32(e,static_cast<std::uint32_t>(i+1));write64(e+8,offsets[i]);write64(e+16,byte_sizes[i]);write32(e+24,counts[i]);write32(e+28,sizes[i]);}
        record_phase(metrics,metric_id::source_checkpoint_snapshot_layout_duration,phase_begin);
        phase_begin=persistence_clock::now();
        std::uint32_t path_offset=0;for(std::uint32_t id=1;id<=count;++id){auto*core=bytes.data()+offsets[0]+std::uint64_t(id-1)*8;write32(core,path_offset);write32(core+4,static_cast<std::uint32_t>(paths[id-1].size()));std::memcpy(bytes.data()+offsets[8]+path_offset,paths[id-1].data(),paths[id-1].size());path_offset+=static_cast<std::uint32_t>(paths[id-1].size());source_physical_state physical;const auto physical_result=manager.get_physical_state(source_id{id},physical);if(!physical_result.ok()&&physical_result.code!=status_code::invalid_state)return physical_result;auto*p=bytes.data()+offsets[1]+std::uint64_t(id-1)*56;if(physical_result.ok()&&physical.presence==source_presence::present){write64(p,physical.observation.write_time_ticks);write64(p+8,physical.observation.size);std::memcpy(p+16,physical.hash.bytes.data(),32);write32(p+48,1);}}
        record_phase(metrics,metric_id::source_checkpoint_source_state_duration,phase_begin);
        phase_begin=persistence_clock::now();
        std::uint32_t edge_at=0;write32(bytes.data()+offsets[2],0);for(std::uint32_t id=1;id<=count;++id){for(auto edge:manager.includes(source_id{id})){write32(bytes.data()+offsets[3]+std::uint64_t(edge_at++)*4,edge.value());}write32(bytes.data()+offsets[2]+std::uint64_t(id)*4,edge_at);}record_phase(metrics,metric_id::source_checkpoint_forward_csr_duration,phase_begin);
        phase_begin=persistence_clock::now();edge_at=0;write32(bytes.data()+offsets[4],0);for(std::uint32_t id=1;id<=count;++id){for(auto edge:manager.dependents(source_id{id})){write32(bytes.data()+offsets[5]+std::uint64_t(edge_at++)*4,edge.value());}write32(bytes.data()+offsets[4]+std::uint64_t(id)*4,edge_at);}record_phase(metrics,metric_id::source_checkpoint_reverse_csr_duration,phase_begin);
        for(std::size_t i=0;i<manager.root_count();++i){source_root root;auto r=manager.get_root(i,root);if(!r.ok())return r;auto*p=bytes.data()+offsets[6]+i*8;write32(p,root.source.value());p[4]=std::byte(root.role==project_item_role::type?0:1);}
        phase_begin=persistence_clock::now();for(std::uint32_t id=1;id<=count;++id){const auto hash=source_path_xxh64(paths[id-1]);const auto fp=source_path_fingerprint(hash);auto at=std::uint32_t(hash)&(capacity-1);while(read32(bytes.data()+offsets[7]+std::uint64_t(at)*8+4))at=(at+1)&(capacity-1);auto*b=bytes.data()+offsets[7]+std::uint64_t(at)*8;write32(b,fp);write32(b+4,id);}record_phase(metrics,metric_id::source_checkpoint_path_index_duration,phase_begin);
        phase_begin=persistence_clock::now();const auto digest=core::sha256({reinterpret_cast<const char*>(bytes.data()+128),bytes.size()-128});std::memcpy(bytes.data()+72,digest.data(),32);write32(bytes.data()+104,0);write32(bytes.data()+104,source_manager_crc32c(std::span<const std::byte>{bytes.data(),128}));record_phase(metrics,metric_id::source_checkpoint_payload_sha256_duration,phase_begin);
        if(metrics){metrics->increment(metric_id::source_checkpoint_source_count,count);metrics->increment(metric_id::source_checkpoint_root_count,manager.root_count());metrics->increment(metric_id::source_checkpoint_forward_edge_count,forward_count);metrics->increment(metric_id::source_checkpoint_reverse_edge_count,reverse_count);metrics->increment(metric_id::source_checkpoint_path_bytes,path_size);metrics->increment(metric_id::source_checkpoint_path_index_capacity,capacity);metrics->increment(metric_id::source_checkpoint_artifact_bytes,bytes.size());metrics->increment(metric_id::source_checkpoint_bytes_hashed,bytes.size()-128);}
        auto temp=target;temp+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());unique_handle file{CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};if(file.value==INVALID_HANDLE_VALUE)return{status_code::persistence_failed};
        phase_begin=persistence_clock::now();std::size_t written=0;while(written<bytes.size()){DWORD amount=0,request=static_cast<DWORD>((std::min)(bytes.size()-written,std::size_t(std::numeric_limits<DWORD>::max())));if(!WriteFile(file.value,bytes.data()+written,request,&amount,nullptr)||!amount){file.close();DeleteFileW(temp.c_str());return{status_code::persistence_failed};}written+=amount;}record_phase(metrics,metric_id::source_checkpoint_write_duration,phase_begin);if(metrics)metrics->increment(metric_id::source_checkpoint_bytes_written,written);
        phase_begin=persistence_clock::now();if(!FlushFileBuffers(file.value)){file.close();DeleteFileW(temp.c_str());return{status_code::persistence_failed};}record_phase(metrics,metric_id::source_checkpoint_flush_duration,phase_begin);file.close();
        status validation;{stable_source_manager_view validation_view;phase_begin=persistence_clock::now();validation=validation_view.open(temp);record_phase(metrics,metric_id::source_checkpoint_reopen_map_duration,phase_begin);}if(!validation.ok()){DeleteFileW(temp.c_str());return validation;}
        DWORD attrs=GetFileAttributesW(target.c_str());BOOL published=attrs!=INVALID_FILE_ATTRIBUTES?ReplaceFileW(target.c_str(),temp.c_str(),nullptr,REPLACEFILE_WRITE_THROUGH,nullptr,nullptr):MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH);if(!published){DeleteFileW(temp.c_str());return{status_code::persistence_failed};}save_metrics.success=true;return{};
    } catch(...) { return{status_code::initialization_failed}; }
}
}
