#pragma once

#include "diagnostic.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace cw::server
{

class diagnostic_buffer
{
public:
    void clear() noexcept;
    void reserve(std::size_t count);
    void emit(diagnostic_record record);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] std::span<const diagnostic_record> records() const noexcept;

    void sort_deterministic();

private:
    std::vector<diagnostic_record> records_;
};

} // namespace cw::server
