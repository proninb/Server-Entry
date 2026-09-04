#pragma once

#include <charconv>
#include <string>
#include <string_view>

namespace cw::server {

// Owns a reusable character buffer used while constructing JSON text.
// json_buffer converts append/allocation failures into boolean status so callers
// can build JSON without propagating exceptions through Server control paths.
class json_buffer {
public:
    void clear() noexcept { buffer.clear(); }
    void reserve(std::size_t count) { buffer.reserve(count); }

    [[nodiscard]] bool append(char value) noexcept {
        try {
            buffer.push_back(value);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool append(std::string_view value) noexcept {
        try {
            buffer.append(value);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    template <typename Integer>
    [[nodiscard]] bool append_integer(Integer value) noexcept {
        char storage[32];
        const auto result = std::to_chars(storage, storage + sizeof(storage), value);

        return result.ec == std::errc{} && append({storage, result.ptr});
    }

    [[nodiscard]] std::string_view view() const noexcept { return buffer; }
    [[nodiscard]] const std::string& string() const noexcept { return buffer; }

private:
    std::string buffer;
};

} // namespace cw::server
