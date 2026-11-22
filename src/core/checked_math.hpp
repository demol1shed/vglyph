#pragma once

// Internal helpers (not installed): overflow-checked arithmetic for buffer sizes.

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

}  // namespace vglyph
