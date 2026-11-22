#pragma once

// Pixel and grid primitives shared by the converter, serializers and
// exporters (SPEC section 3.2).

#include <cstdint>
#include <vector>

namespace vglyph {

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct Cell {
    char32_t codepoint = U' ';
    Rgb fg;
    Rgb bg;
};

// Row-major grid of cells. The converter writes into a caller-owned frame
// and resizes `cells` only when the grid size changes, so a playback loop
// reuses one buffer.
struct AsciiFrame {
    std::uint32_t cols = 0;
    std::uint32_t rows = 0;
    std::vector<Cell> cells;       // length cols * rows
    std::int64_t pts_us = 0;       // 0 for still images
    std::int64_t duration_us = 0;  // 0 for still images
};

}  // namespace vglyph
