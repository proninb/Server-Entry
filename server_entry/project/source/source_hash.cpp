#include "source_hash.hpp"

#include "../../core/hash/sha256.hpp"

namespace cw::server
{
source_content_hash hash_source_content(std::string_view bytes) noexcept
{
    return {core::sha256(bytes)};
}
} // namespace cw::server
