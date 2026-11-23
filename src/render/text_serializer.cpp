#include "vglyph/text_serializer.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <string_view>

#include "checked_math.hpp"
#include "vglyph/utf8.hpp"

namespace vglyph {

namespace {

constexpr std::string_view kFgPrefix = "\x1b[38;2;";
constexpr std::string_view kBgPrefix = "\x1b[48;2;";
constexpr std::string_view kReset = "\x1b[0m";

constexpr std::uint64_t kMaxUtf8Bytes = 4;
constexpr std::uint64_t kMaxSgrBytes = 19;  // ESC[38;2;255;255;255m
constexpr std::uint64_t kLineBreakBytes = 1;

[[nodiscard]] bool same(Rgb a, Rgb b) noexcept { return a.r == b.r && a.g == b.g && a.b == b.b; }

// Writes v as decimal digits at p and returns the new end.
[[nodiscard]] char* put_byte(char* p, unsigned v) noexcept {
    if (v >= 100) {
        *p++ = static_cast<char>('0' + v / 100);
        v %= 100;
        *p++ = static_cast<char>('0' + v / 10);
    } else if (v >= 10) {
        *p++ = static_cast<char>('0' + v / 10);
    }
    *p++ = static_cast<char>('0' + v % 10);
    return p;
}

void append_sgr(std::string& out, std::string_view prefix, Rgb c) {
    std::array<char, 24> buf{};  // prefix (7) + "255;255;255" (11) + 'm' (1) = 19
    char* p = buf.data();
    for (const char ch : prefix) {
        *p++ = ch;
    }
    p = put_byte(p, c.r);
    *p++ = ';';
    p = put_byte(p, c.g);
    *p++ = ';';
    p = put_byte(p, c.b);
    *p++ = 'm';
    out.append(buf.data(), static_cast<std::size_t>(p - buf.data()));
}

// Checks cells.size() == cols * rows without overflow.
[[nodiscard]] Status check_shape(const AsciiFrame& frame) {
    std::uint64_t expected = 0;
    if (!checked_mul(frame.cols, frame.rows, expected) || expected != frame.cells.size()) {
        return make_error(ErrorCode::InvalidArgument, "AsciiFrame cells size is not cols * rows");
    }
    return Status::success();
}

// Bytes for `cells` cells costing `per_cell` each plus `rows` rows costing
// `per_row` each, overflow-checked and limited to std::size_t.
[[nodiscard]] Result<std::size_t> capacity(std::uint64_t cols, std::uint64_t rows,
                                           std::uint64_t per_cell, std::uint64_t per_row) {
    const Error too_big = make_error(ErrorCode::LimitExceeded, "serialized frame size overflows");
    std::uint64_t cells = 0;
    std::uint64_t cell_bytes = 0;
    std::uint64_t row_bytes = 0;
    if (!checked_mul(cols, rows, cells) || !checked_mul(cells, per_cell, cell_bytes) ||
        !checked_mul(rows, per_row, row_bytes) ||
        cell_bytes > std::numeric_limits<std::uint64_t>::max() - row_bytes) {
        return too_big;
    }
    const std::uint64_t total = cell_bytes + row_bytes;
    if (total > std::numeric_limits<std::size_t>::max()) {
        return too_big;
    }
    return static_cast<std::size_t>(total);
}

}  // namespace

Status append_text(const AsciiFrame& frame, std::string& out) {
    if (const Status s = check_shape(frame); !s.ok()) {
        return s;
    }
    const Cell* cell = frame.cells.data();
    for (std::uint32_t row = 0; row < frame.rows; ++row) {
        for (std::uint32_t col = 0; col < frame.cols; ++col, ++cell) {
            append_utf8(cell->codepoint, out);
        }
        if (row + 1 < frame.rows) {
            out.push_back('\n');
        }
    }
    return Status::success();
}

Status append_ansi(const AsciiFrame& frame, std::string& out) {
    if (const Status s = check_shape(frame); !s.ok()) {
        return s;
    }
    const Cell* cell = frame.cells.data();
    for (std::uint32_t row = 0; row < frame.rows; ++row) {
        Rgb prev_fg;
        Rgb prev_bg;
        for (std::uint32_t col = 0; col < frame.cols; ++col, ++cell) {
            // The previous row ended with a reset, so the first cell sets both colors.
            if (col == 0 || !same(cell->fg, prev_fg)) {
                append_sgr(out, kFgPrefix, cell->fg);
            }
            if (col == 0 || !same(cell->bg, prev_bg)) {
                append_sgr(out, kBgPrefix, cell->bg);
            }
            append_utf8(cell->codepoint, out);
            prev_fg = cell->fg;
            prev_bg = cell->bg;
        }
        out.append(kReset);
        if (row + 1 < frame.rows) {
            out.push_back('\n');
        }
    }
    return Status::success();
}

Result<std::size_t> text_capacity(std::uint32_t cols, std::uint32_t rows) {
    return capacity(cols, rows, kMaxUtf8Bytes, kLineBreakBytes);
}

Result<std::size_t> ansi_capacity(std::uint32_t cols, std::uint32_t rows) {
    return capacity(cols, rows, 2 * kMaxSgrBytes + kMaxUtf8Bytes, kReset.size() + kLineBreakBytes);
}

}  // namespace vglyph
