// Requirements: UR-RES-7
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace slideio::processor::detail {

// True when `text` is well-formed UTF-8: no stray continuation byte, no truncated
// sequence, no overlong form, no surrogate (U+D800 to U+DFFF), nothing above U+10FFFF.
// Header-only, so the data and the engine library can each use it.
inline bool isValidUtf8(const std::string& text) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t length = 0;
        uint32_t code = 0;
        uint32_t minimum = 0;
        if ((c & 0xE0) == 0xC0) {
            length = 2;
            code = c & 0x1Fu;
            minimum = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            code = c & 0x0Fu;
            minimum = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            code = c & 0x07u;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (n - i < length) return false;
        for (size_t k = 1; k < length; ++k) {
            const unsigned char next = p[i + k];
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3Fu);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
        i += length;
    }
    return true;
}

} // namespace slideio::processor::detail
