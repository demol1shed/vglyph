#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "test_framework.hpp"
#include "vglyph/text_serializer.hpp"

using vglyph::AsciiFrame;
using vglyph::Cell;
using vglyph::ErrorCode;
using vglyph::Rgb;

namespace {

constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kBlack{0, 0, 0};

// A frame of cols x rows cells, all `fill`.
AsciiFrame make_frame(std::uint32_t cols, std::uint32_t rows, const Cell& fill) {
    AsciiFrame f;
    f.cols = cols;
    f.rows = rows;
    f.cells.assign(static_cast<std::size_t>(cols) * rows, fill);
    return f;
}

Cell cell(char32_t cp, Rgb fg, Rgb bg) {
    Cell c;
    c.codepoint = cp;
    c.fg = fg;
    c.bg = bg;
    return c;
}

std::string text_of(const AsciiFrame& f) {
    std::string out;
    VGLYPH_CHECK(vglyph::append_text(f, out).ok());
    return out;
}

std::string ansi_of(const AsciiFrame& f) {
    std::string out;
    VGLYPH_CHECK(vglyph::append_ansi(f, out).ok());
    return out;
}

std::size_t count(const std::string& haystack, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

VGLYPH_TEST(text_drops_colors_and_has_no_trailing_newline) {
    AsciiFrame f = make_frame(2, 2, cell(U'a', kRed, kGreen));
    f.cells[1].codepoint = U'b';
    f.cells[2].codepoint = U'c';
    f.cells[3].codepoint = U'd';
    VGLYPH_CHECK(text_of(f) == "ab\ncd");
}

VGLYPH_TEST(text_encodes_half_block_as_utf8) {
    const AsciiFrame f = make_frame(2, 1, cell(0x2580, kRed, kGreen));
    VGLYPH_CHECK(text_of(f) == "\xE2\x96\x80\xE2\x96\x80");
}

VGLYPH_TEST(text_writes_replacement_for_invalid_codepoint) {
    const AsciiFrame f = make_frame(1, 1, cell(0x110000, kRed, kGreen));
    VGLYPH_CHECK(text_of(f) == "\xEF\xBF\xBD");
}

VGLYPH_TEST(empty_frame_serializes_to_nothing) {
    const AsciiFrame f;
    VGLYPH_CHECK(text_of(f).empty());
    VGLYPH_CHECK(ansi_of(f).empty());
}

VGLYPH_TEST(ansi_solid_row_writes_each_color_once) {
    const AsciiFrame f = make_frame(3, 1, cell(U'x', Rgb{1, 22, 133}, Rgb{4, 5, 6}));
    VGLYPH_CHECK(ansi_of(f) ==
                 "\x1b[38;2;1;22;133m"
                 "\x1b[48;2;4;5;6m"
                 "xxx"
                 "\x1b[0m");
}

VGLYPH_TEST(ansi_writes_only_the_color_that_changed) {
    AsciiFrame f = make_frame(3, 1, cell(U'x', kRed, kBlack));
    f.cells[1].fg = kGreen;  // fg changes, bg stays
    f.cells[2].fg = kGreen;
    f.cells[2].bg = kRed;  // bg changes, fg stays
    VGLYPH_CHECK(ansi_of(f) ==
                 "\x1b[38;2;255;0;0m"
                 "\x1b[48;2;0;0;0m"
                 "x"
                 "\x1b[38;2;0;255;0m"
                 "x"
                 "\x1b[48;2;255;0;0m"
                 "x"
                 "\x1b[0m");
}

VGLYPH_TEST(ansi_restarts_colors_on_every_row) {
    const AsciiFrame f = make_frame(2, 3, cell(U'x', kRed, kBlack));
    const std::string out = ansi_of(f);
    VGLYPH_CHECK(count(out, "\x1b[38;2;") == 3);  // one fg per row, none inside a row
    VGLYPH_CHECK(count(out, "\x1b[48;2;") == 3);
    VGLYPH_CHECK(count(out, "\x1b[0m") == 3);     // reset at the end of every row
    VGLYPH_CHECK(count(out, "\x1b[0m\n\x1b[38;2;") == 2);
}

VGLYPH_TEST(ansi_has_no_trailing_newline) {
    const AsciiFrame f = make_frame(2, 2, cell(U'x', kRed, kBlack));
    const std::string out = ansi_of(f);
    VGLYPH_CHECK(out.size() > 4);
    VGLYPH_CHECK(out.compare(out.size() - 4, 4, "\x1b[0m") == 0);
}

VGLYPH_TEST(serializers_reject_a_mismatched_cell_count) {
    AsciiFrame f = make_frame(2, 2, cell(U'x', kRed, kBlack));
    f.cells.pop_back();
    std::string out = "keep";
    const vglyph::Status text = vglyph::append_text(f, out);
    const vglyph::Status ansi = vglyph::append_ansi(f, out);
    VGLYPH_CHECK(!text.ok() && text.error().code == ErrorCode::InvalidArgument);
    VGLYPH_CHECK(!ansi.ok() && ansi.error().code == ErrorCode::InvalidArgument);
    VGLYPH_CHECK(out == "keep");
}

VGLYPH_TEST(ansi_capacity_matches_the_worst_case_exactly) {
    // Alternate two 3-digit colors in fg and bg and use a 4-byte glyph: every
    // cell writes both SGRs, which is the largest possible output.
    AsciiFrame f = make_frame(4, 3, cell(0x1F600, Rgb{255, 255, 255}, Rgb{255, 255, 255}));
    for (std::size_t i = 1; i < f.cells.size(); i += 2) {
        f.cells[i].fg = Rgb{254, 254, 254};
        f.cells[i].bg = Rgb{254, 254, 254};
    }
    const vglyph::Result<std::size_t> cap = vglyph::ansi_capacity(f.cols, f.rows);
    VGLYPH_CHECK(cap.value_if() != nullptr);
    const std::string out = ansi_of(f);
    // Capacity counts a '\n' after every row, the output after all but the last.
    VGLYPH_CHECK(cap.value_if() != nullptr && out.size() + 1 == *cap.value_if());
}

VGLYPH_TEST(text_capacity_matches_the_worst_case_exactly) {
    const AsciiFrame f = make_frame(4, 3, cell(0x1F600, kRed, kBlack));
    const vglyph::Result<std::size_t> cap = vglyph::text_capacity(f.cols, f.rows);
    VGLYPH_CHECK(cap.value_if() != nullptr);
    VGLYPH_CHECK(cap.value_if() != nullptr && text_of(f).size() + 1 == *cap.value_if());
}

VGLYPH_TEST(reserved_buffer_is_reused_without_reallocation) {
    const AsciiFrame f = make_frame(8, 4, cell(0x2580, kRed, kGreen));
    const vglyph::Result<std::size_t> cap = vglyph::ansi_capacity(f.cols, f.rows);
    VGLYPH_CHECK(cap.value_if() != nullptr);
    if (cap.value_if() == nullptr) {
        return;
    }
    std::string out;
    out.reserve(*cap.value_if());
    const char* data = out.data();
    const std::size_t reserved = out.capacity();
    for (int i = 0; i < 3; ++i) {
        out.clear();
        VGLYPH_CHECK(vglyph::append_ansi(f, out).ok());
        VGLYPH_CHECK(out.data() == data);
        VGLYPH_CHECK(out.capacity() == reserved);
    }
}

VGLYPH_TEST(capacity_reports_overflow_as_limit_exceeded) {
    const std::uint32_t big = std::numeric_limits<std::uint32_t>::max();
    const vglyph::Result<std::size_t> text = vglyph::text_capacity(big, big);
    const vglyph::Result<std::size_t> ansi = vglyph::ansi_capacity(big, big);
    VGLYPH_CHECK(text.error_if() != nullptr && text.error_if()->code == ErrorCode::LimitExceeded);
    VGLYPH_CHECK(ansi.error_if() != nullptr && ansi.error_if()->code == ErrorCode::LimitExceeded);
}

VGLYPH_TEST_MAIN()
