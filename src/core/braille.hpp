#pragma once

// The Unicode Braille Patterns block and its dot layout (SPEC section 4.3),
// shared by the converter, the glyph atlas and the rasterizer.

#include <array>
#include <cstdint>

namespace vglyph {

inline constexpr char32_t kBrailleBlank = 0x2800;  // no dot raised
inline constexpr char32_t kBrailleLast = 0x28FF;   // all eight dots raised

// Mask bit of the dot at [row][column] of a cell; codepoint = kBrailleBlank + mask.
inline constexpr std::array<std::array<std::uint8_t, 2>, 4> kBrailleDotBit{{
    {0x01, 0x08},
    {0x02, 0x10},
    {0x04, 0x20},
    {0x40, 0x80},
}};

[[nodiscard]] constexpr bool is_braille(char32_t codepoint) noexcept {
    return codepoint >= kBrailleBlank && codepoint <= kBrailleLast;
}

}  // namespace vglyph
