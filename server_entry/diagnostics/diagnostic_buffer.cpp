#include "diagnostic_buffer.hpp"

#include <algorithm>
#include <string_view>
#include <tuple>
#include <utility>

namespace cw::server
{

void diagnostic_buffer::clear() noexcept
{
    records_.clear();
}

void diagnostic_buffer::reserve(std::size_t count)
{
    records_.reserve(count);
}

void diagnostic_buffer::emit(diagnostic_record record)
{
    records_.push_back(std::move(record));
}

bool diagnostic_buffer::empty() const noexcept
{
    return records_.empty();
}

bool diagnostic_buffer::has_errors() const noexcept
{
    return std::ranges::any_of(records_, [](const diagnostic_record& record) {
        return record.severity == diagnostic_severity::error ||
               record.severity == diagnostic_severity::fatal;
    });
}

std::span<const diagnostic_record> diagnostic_buffer::records() const noexcept
{
    return records_;
}

void diagnostic_buffer::sort_deterministic()
{
    std::ranges::sort(records_, {}, [](const diagnostic_record& record) {
        return std::tuple{record.location.source.value(), record.location.offset,
                          record.location.length, record.id.value(), record.severity,
                          record.operation.value(), std::string_view{record.detail}};
    });
}

} // namespace cw::server
