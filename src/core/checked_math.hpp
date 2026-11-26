#pragma once

// Internal helpers (not installed): overflow-checked arithmetic for buffer sizes
// and media timestamps.

#include <cstdint>

namespace vglyph {

// Stores a * b in `out` and returns true, or returns false on 64-bit overflow
// (leaving `out` untouched).
[[nodiscard]] inline bool checked_mul(std::uint64_t a, std::uint64_t b,
                                      std::uint64_t& out) noexcept {
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }
    out = a * b;
    return true;
}

// The same for signed a + b.
[[nodiscard]] inline bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return false;
    }
    out = a + b;
    return true;
}

// The same for signed a - b.
[[nodiscard]] inline bool checked_sub(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
        return false;
    }
    out = a - b;
    return true;
}

}  // namespace vglyph
