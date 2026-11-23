#pragma once

// Text (.txt) and ANSI (.ans) serialization of an AsciiFrame (SPEC section 6.1).

#include <cstddef>
#include <cstdint>
#include <string>

#include "vglyph/error.hpp"
#include "vglyph/frame.hpp"

namespace vglyph {

// Both functions append to `out` (they never clear it) and fail with
// InvalidArgument, leaving `out` untouched, when frame.cells.size() is not
// cols * rows. Rows are separated by '\n'; the last row has no line break, so
// a player can draw a full-height frame without scrolling. A file writer adds
// the final '\n'.
//
// After `out.reserve(text_capacity(...))` (or ansi_capacity) and a clear(),
// serializing a frame of that size performs no allocation.

// Plain UTF-8 text: codepoints only, colors dropped.
[[nodiscard]] Status append_text(const AsciiFrame& frame, std::string& out);

// UTF-8 text with 24-bit SGR colors. Every row starts by writing fg and bg;
// inside a row an SGR is written only when the color differs from the
// previous cell. Every row ends with ESC[0m so that no color bleeds across a
// line break.
[[nodiscard]] Status append_ansi(const AsciiFrame& frame, std::string& out);

// Worst-case byte size of append_text / append_ansi output for a grid.
// LimitExceeded if the size does not fit in std::size_t.
[[nodiscard]] Result<std::size_t> text_capacity(std::uint32_t cols, std::uint32_t rows);
[[nodiscard]] Result<std::size_t> ansi_capacity(std::uint32_t cols, std::uint32_t rows);

}  // namespace vglyph
