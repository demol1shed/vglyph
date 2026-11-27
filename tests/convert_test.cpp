#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "test_framework.hpp"
#include "vglyph/convert.hpp"

using vglyph::AsciiFrame;
using vglyph::Cell;
using vglyph::ColorMode;
using vglyph::ConvertConfig;
using vglyph::ErrorCode;
using vglyph::RenderMode;
using vglyph::Rgb;
using vglyph::Status;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr char32_t kHalfBlock = 0x2580;  // U+2580, numeric to keep the source ASCII
constexpr char32_t kBraille = 0x2800;    // U+2800, the pattern without dots

bool same(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

bool same(const Cell& a, const Cell& b) {
    return a.codepoint == b.codepoint && same(a.fg, b.fg) && same(a.bg, b.bg);
}

bool same(const AsciiFrame& a, const AsciiFrame& b) {
    if (a.cols != b.cols || a.rows != b.rows || a.cells.size() != b.cells.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        if (!same(a.cells[i], b.cells[i])) {
            return false;
        }
    }
    return true;
}

bool has_code(const Status& s, ErrorCode code) { return !s.ok() && s.error().code == code; }

// A width x height buffer where every pixel is `color`.
Bytes solid(std::uint32_t width, std::uint32_t height, Rgb color) {
    Bytes b;
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        b.push_back(color.r);
        b.push_back(color.g);
        b.push_back(color.b);
    }
    return b;
}

// A buffer of `width` columns where row y is solid gray `row_grays[y]`.
Bytes gray_rows(std::uint32_t width, std::initializer_list<std::uint8_t> row_grays) {
    Bytes b;
    for (const std::uint8_t g : row_grays) {
        for (std::uint32_t x = 0; x < width; ++x) {
            b.push_back(g);
            b.push_back(g);
            b.push_back(g);
        }
    }
    return b;
}

// Two rows of `width` pixels: the top row is `top`, the bottom row is `bottom`.
Bytes two_rows(std::uint32_t width, Rgb top, Rgb bottom) {
    Bytes b = solid(width, 1, top);
    const Bytes lower = solid(width, 1, bottom);
    b.insert(b.end(), lower.begin(), lower.end());
    return b;
}

ConvertConfig config4() {
    ConvertConfig c;
    c.target_cols = 4;
    return c;
}

Status run(const Bytes& buf, std::uint32_t w, std::uint32_t h, const ConvertConfig& c,
           AsciiFrame& out) {
    return vglyph::convert(buf, w, h, c, out);
}

// First cell's fg.r after converting one solid gray 4x1 row; isolates the preprocessing.
std::uint8_t preprocessed(std::uint8_t gray, const ConvertConfig& c) {
    AsciiFrame out;
    const Status s = run(gray_rows(4, {gray}), 4, 1, c, out);
    VGLYPH_CHECK(s.ok());
    return out.cells.empty() ? std::uint8_t{0} : out.cells[0].fg.r;
}

// A width x height buffer whose pixel (x, y) is the gray `gray_at(x, y)`.
template <class F>
Bytes gray_plane(std::uint32_t width, std::uint32_t height, F gray_at) {
    Bytes b;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t g = gray_at(x, y);
            b.push_back(g);
            b.push_back(g);
            b.push_back(g);
        }
    }
    return b;
}

// A Sobel config whose ramp shares no glyph with the four strokes.
ConvertConfig sobel_config(std::uint32_t cols) {
    ConvertConfig c;
    c.target_cols = cols;
    c.render_mode = RenderMode::SobelAscii;
    c.charset = U" #";
    return c;
}

AsciiFrame converted(const Bytes& rgb, std::uint32_t w, std::uint32_t h, const ConvertConfig& c) {
    AsciiFrame out;
    VGLYPH_CHECK(vglyph::convert(rgb, w, h, c, out).ok());
    return out;
}

// The frame's glyphs, rows joined without a separator.
std::u32string glyphs_of(const AsciiFrame& frame) {
    std::u32string s;
    for (const Cell& cell : frame.cells) {
        s += cell.codepoint;
    }
    return s;
}

ConvertConfig braille_config(ColorMode color) {
    ConvertConfig c = config4();
    c.render_mode = RenderMode::Braille;
    c.color_mode = color;
    c.bg_color = Rgb{1, 2, 3};
    return c;
}

void set_pixel(Bytes& rgb, std::uint32_t width, std::uint32_t x, std::uint32_t y, Rgb color) {
    const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3;
    rgb[at] = color.r;
    rgb[at + 1] = color.g;
    rgb[at + 2] = color.b;
}

// One row of four Braille cells whose first cell has two bright pixels (dots
// 0x01 and 0x80) among dim blue ones.
Bytes two_dot_plane() {
    Bytes rgb = solid(8, 4, Rgb{0, 0, 100});
    set_pixel(rgb, 8, 0, 0, Rgb{255, 201, 1});
    set_pixel(rgb, 8, 1, 3, Rgb{255, 255, 255});
    return rgb;
}

// First cell of an 8x4 Braille plane.
Cell braille_cell(const Bytes& rgb, const ConvertConfig& c) {
    const AsciiFrame out = converted(rgb, 8, 4, c);
    return out.cells.empty() ? Cell{} : out.cells[0];
}

}  // namespace

VGLYPH_TEST(ascii_maps_luminance_onto_ramp) {
    // Default ramp " .:-=+*#%@" (N = 10): gray / 255 * 10 -> 0, 2.5, 5.0, 10 (capped to 9).
    AsciiFrame out;
    VGLYPH_CHECK(run(gray_rows(4, {0, 64, 128, 255}), 4, 4, config4(), out).ok());
    VGLYPH_CHECK(out.cols == 4);
    VGLYPH_CHECK(out.rows == 4);
    VGLYPH_CHECK(out.cells.size() == 16);
    VGLYPH_CHECK(out.cells[0].codepoint == U' ');
    VGLYPH_CHECK(out.cells[4].codepoint == U':');
    VGLYPH_CHECK(out.cells[8].codepoint == U'+');
    VGLYPH_CHECK(out.cells[12].codepoint == U'@');
    VGLYPH_CHECK(out.cells[15].codepoint == U'@');
}

VGLYPH_TEST(ascii_two_codepoint_ramp) {
    ConvertConfig c = config4();
    c.charset = U"ab";
    AsciiFrame out;
    VGLYPH_CHECK(run(gray_rows(4, {0, 64, 128, 255}), 4, 4, c, out).ok());
    VGLYPH_CHECK(out.cells[0].codepoint == U'a');
    VGLYPH_CHECK(out.cells[4].codepoint == U'a');
    VGLYPH_CHECK(out.cells[8].codepoint == U'b');
    VGLYPH_CHECK(out.cells[12].codepoint == U'b');
}

VGLYPH_TEST(ascii_color_modes) {
    // Pixel (10, 200, 30): Y' = 0.5778 -> glyph '+', gray 147; fg/4 = (3, 50, 8).
    const Bytes buf = solid(4, 1, Rgb{10, 200, 30});
    ConvertConfig c = config4();
    c.bg_color = Rgb{1, 2, 3};
    AsciiFrame out;

    c.color_mode = ColorMode::Mono;
    VGLYPH_CHECK(run(buf, 4, 1, c, out).ok());
    VGLYPH_CHECK(out.cells[0].codepoint == U'+');
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{255, 255, 255}));
    VGLYPH_CHECK(same(out.cells[0].bg, Rgb{1, 2, 3}));

    c.color_mode = ColorMode::Grayscale;
    VGLYPH_CHECK(run(buf, 4, 1, c, out).ok());
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{147, 147, 147}));
    VGLYPH_CHECK(same(out.cells[0].bg, Rgb{1, 2, 3}));

    c.color_mode = ColorMode::TrueColorFg;
    VGLYPH_CHECK(run(buf, 4, 1, c, out).ok());
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{10, 200, 30}));
    VGLYPH_CHECK(same(out.cells[0].bg, Rgb{1, 2, 3}));

    c.color_mode = ColorMode::TrueColorFgBg;
    VGLYPH_CHECK(run(buf, 4, 1, c, out).ok());
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{10, 200, 30}));
    VGLYPH_CHECK(same(out.cells[0].bg, Rgb{3, 50, 8}));
}

VGLYPH_TEST(half_block_color_modes) {
    // Top A = (10, 200, 30): Y' = 0.5778, gray 147. Bottom B = (250, 20, 100): Y' = 0.2928, gray 75.
    const Rgb a{10, 200, 30};
    const Rgb b{250, 20, 100};
    ConvertConfig c = config4();
    c.render_mode = RenderMode::HalfBlock;
    c.bg_color = Rgb{1, 2, 3};
    AsciiFrame out;

    // TrueColorFg and TrueColorFgBg behave the same: fg = top, bg = bottom, no darkening.
    for (const ColorMode mode : {ColorMode::TrueColorFg, ColorMode::TrueColorFgBg}) {
        c.color_mode = mode;
        VGLYPH_CHECK(run(two_rows(4, a, b), 4, 2, c, out).ok());
        VGLYPH_CHECK(out.cols == 4);
        VGLYPH_CHECK(out.rows == 1);
        VGLYPH_CHECK(out.cells[0].codepoint == kHalfBlock);
        VGLYPH_CHECK(same(out.cells[0].fg, a));
        VGLYPH_CHECK(same(out.cells[0].bg, b));
    }

    c.color_mode = ColorMode::Grayscale;
    VGLYPH_CHECK(run(two_rows(4, a, b), 4, 2, c, out).ok());
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{147, 147, 147}));
    VGLYPH_CHECK(same(out.cells[0].bg, Rgb{75, 75, 75}));

    // Mono thresholds each half at Y' >= 0.5: set -> white, unset -> bg_color.
    c.color_mode = ColorMode::Mono;
    VGLYPH_CHECK(run(two_rows(4, a, b), 4, 2, c, out).ok());
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{255, 255, 255}));
    VGLYPH_CHECK(same(out.cells[0].bg, Rgb{1, 2, 3}));
    VGLYPH_CHECK(run(two_rows(4, b, a), 4, 2, c, out).ok());
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{1, 2, 3}));
    VGLYPH_CHECK(same(out.cells[0].bg, Rgb{255, 255, 255}));
}

VGLYPH_TEST(half_block_pairs_rows_top_and_bottom) {
    // Pixel rows 0..5 have gray 0, 40, 80, 120, 160, 200 -> cell rows pair (0,40), (80,120), (160,200).
    ConvertConfig c = config4();
    c.render_mode = RenderMode::HalfBlock;
    AsciiFrame out;
    VGLYPH_CHECK(run(gray_rows(4, {0, 40, 80, 120, 160, 200}), 4, 6, c, out).ok());
    VGLYPH_CHECK(out.rows == 3);
    VGLYPH_CHECK(out.cells.size() == 12);
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{0, 0, 0}));
    VGLYPH_CHECK(same(out.cells[0].bg, Rgb{40, 40, 40}));
    VGLYPH_CHECK(same(out.cells[4].fg, Rgb{80, 80, 80}));
    VGLYPH_CHECK(same(out.cells[4].bg, Rgb{120, 120, 120}));
    VGLYPH_CHECK(same(out.cells[11].fg, Rgb{160, 160, 160}));
    VGLYPH_CHECK(same(out.cells[11].bg, Rgb{200, 200, 200}));
}

VGLYPH_TEST(invert_flips_luminance_only) {
    ConvertConfig c = config4();
    c.invert = true;
    AsciiFrame out;
    VGLYPH_CHECK(run(gray_rows(4, {0, 255}), 4, 2, c, out).ok());
    VGLYPH_CHECK(out.cells[0].codepoint == U'@');
    VGLYPH_CHECK(out.cells[4].codepoint == U' ');

    // Grayscale follows Y': 1 - 64/255 -> 191.
    c.color_mode = ColorMode::Grayscale;
    VGLYPH_CHECK(run(gray_rows(4, {64}), 4, 1, c, out).ok());
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{191, 191, 191}));

    // TrueColor fg keeps the preprocessed color (SPEC section 4.3).
    c.color_mode = ColorMode::TrueColorFg;
    VGLYPH_CHECK(run(solid(4, 1, Rgb{10, 200, 30}), 4, 1, c, out).ok());
    VGLYPH_CHECK(same(out.cells[0].fg, Rgb{10, 200, 30}));
}

VGLYPH_TEST(brightness_shifts_and_clamps) {
    ConvertConfig c = config4();
    c.brightness = 0.25f;  // 128/255 + 0.25 -> 191.75 -> 192
    VGLYPH_CHECK(preprocessed(128, c) == 192);
    c.brightness = 1.0f;
    VGLYPH_CHECK(preprocessed(0, c) == 255);
    c.brightness = -1.0f;
    VGLYPH_CHECK(preprocessed(255, c) == 0);
}

VGLYPH_TEST(contrast_scales_around_mid_gray_and_clamps) {
    ConvertConfig c = config4();
    c.contrast = 0.5f;  // 0.5 * (100 - 127.5) + 127.5 = 113.75 -> 114
    VGLYPH_CHECK(preprocessed(100, c) == 114);
    c.contrast = 3.0f;  // 3 * (120 - 127.5) + 127.5 = 105
    VGLYPH_CHECK(preprocessed(120, c) == 105);
    VGLYPH_CHECK(preprocessed(10, c) == 0);
    VGLYPH_CHECK(preprocessed(250, c) == 255);
}

VGLYPH_TEST(gamma_applies_before_contrast_and_brightness) {
    ConvertConfig c = config4();
    c.gamma = 2.0f;  // (128/255)^2 = 0.25196 -> 64.25 -> 64
    VGLYPH_CHECK(preprocessed(128, c) == 64);
    c.gamma = 0.5f;  // sqrt(64/255) = 0.50098 -> 127.75 -> 128
    VGLYPH_CHECK(preprocessed(64, c) == 128);
    c.gamma = 2.0f;
    c.brightness = 0.25f;  // 0.25196 + 0.25 = 0.50196 -> 128
    VGLYPH_CHECK(preprocessed(128, c) == 128);
}

VGLYPH_TEST(rejects_invalid_arguments_and_leaves_output_untouched) {
    AsciiFrame out;
    VGLYPH_CHECK(run(gray_rows(4, {0, 255}), 4, 2, config4(), out).ok());
    out.pts_us = 123;
    out.duration_us = 456;
    AsciiFrame before = out;

    // Wrong width.
    VGLYPH_CHECK(has_code(run(gray_rows(5, {0}), 5, 1, config4(), out), ErrorCode::InvalidArgument));

    // Zero height, and a HalfBlock buffer with an odd height.
    VGLYPH_CHECK(has_code(run(Bytes{}, 4, 0, config4(), out), ErrorCode::InvalidArgument));
    ConvertConfig half = config4();
    half.render_mode = RenderMode::HalfBlock;
    VGLYPH_CHECK(has_code(run(gray_rows(4, {0, 0, 0}), 4, 3, half, out), ErrorCode::InvalidArgument));

    // Buffer one byte short and one byte long.
    Bytes buf = gray_rows(4, {0, 0});
    buf.pop_back();
    VGLYPH_CHECK(has_code(run(buf, 4, 2, config4(), out), ErrorCode::InvalidArgument));
    buf.push_back(0);
    buf.push_back(0);
    VGLYPH_CHECK(has_code(run(buf, 4, 2, config4(), out), ErrorCode::InvalidArgument));

    // Empty span for a non-empty grid.
    VGLYPH_CHECK(has_code(vglyph::convert(std::span<const std::uint8_t>{}, 4, 2, config4(), out),
                          ErrorCode::InvalidArgument));

    // target_rows that disagrees with the buffer.
    ConvertConfig rows = config4();
    rows.target_rows = 3;
    VGLYPH_CHECK(has_code(run(gray_rows(4, {0, 0}), 4, 2, rows, out), ErrorCode::InvalidArgument));

    // Invalid config.
    ConvertConfig bad = config4();
    bad.gamma = 0.0f;
    VGLYPH_CHECK(has_code(run(gray_rows(4, {0, 0}), 4, 2, bad, out), ErrorCode::InvalidArgument));

    VGLYPH_CHECK(same(out, before));
    VGLYPH_CHECK(out.pts_us == 123);
    VGLYPH_CHECK(out.duration_us == 456);
}

VGLYPH_TEST(rejects_grids_over_the_row_cap) {
    AsciiFrame out;
    const Bytes tall = solid(4, 1025, Rgb{0, 0, 0});
    VGLYPH_CHECK(has_code(run(tall, 4, 1025, config4(), out), ErrorCode::LimitExceeded));
    VGLYPH_CHECK(out.cells.empty());

    // Exactly 1024 rows is fine, and so is HalfBlock with 2048 pixel rows.
    VGLYPH_CHECK(run(solid(4, 1024, Rgb{0, 0, 0}), 4, 1024, config4(), out).ok());
    ConvertConfig half = config4();
    half.render_mode = RenderMode::HalfBlock;
    VGLYPH_CHECK(run(solid(4, 2048, Rgb{0, 0, 0}), 4, 2048, half, out).ok());
    VGLYPH_CHECK(out.rows == 1024);
    VGLYPH_CHECK(has_code(run(solid(4, 2050, Rgb{0, 0, 0}), 4, 2050, half, out),
                          ErrorCode::LimitExceeded));
}

VGLYPH_TEST(accepts_matching_target_rows) {
    ConvertConfig c = config4();
    c.target_rows = 2;
    c.render_mode = RenderMode::HalfBlock;
    AsciiFrame out;
    VGLYPH_CHECK(run(gray_rows(4, {0, 0, 0, 0}), 4, 4, c, out).ok());
    VGLYPH_CHECK(out.rows == 2);
}

VGLYPH_TEST(conversion_is_deterministic) {
    const Bytes buf = gray_rows(4, {0, 50, 100, 150, 200, 255});
    ConvertConfig c = config4();
    c.color_mode = ColorMode::TrueColorFgBg;
    AsciiFrame a;
    AsciiFrame b;
    VGLYPH_CHECK(run(buf, 4, 6, c, a).ok());
    VGLYPH_CHECK(run(buf, 4, 6, c, b).ok());
    VGLYPH_CHECK(same(a, b));
}

VGLYPH_TEST(frame_buffer_is_reused_and_timing_is_preserved) {
    const Bytes buf = gray_rows(4, {0, 255});
    AsciiFrame out;
    VGLYPH_CHECK(run(buf, 4, 2, config4(), out).ok());
    out.pts_us = 123;
    out.duration_us = 456;
    const Cell* storage = out.cells.data();
    for (int i = 0; i < 3; ++i) {
        VGLYPH_CHECK(run(buf, 4, 2, config4(), out).ok());
        VGLYPH_CHECK(out.cells.data() == storage);
    }
    VGLYPH_CHECK(out.pts_us == 123);
    VGLYPH_CHECK(out.duration_us == 456);

    // A different grid size resizes the frame.
    VGLYPH_CHECK(run(gray_rows(4, {0, 0, 0}), 4, 3, config4(), out).ok());
    VGLYPH_CHECK(out.rows == 3);
    VGLYPH_CHECK(out.cells.size() == 12);
}

VGLYPH_TEST(sobel_vertical_step_draws_bars_and_keeps_ascii_colors) {
    const Bytes rgb = gray_plane(4, 4, [](std::uint32_t x, std::uint32_t) {
        return static_cast<std::uint8_t>(x < 2 ? 0 : 255);
    });
    ConvertConfig c = sobel_config(4);
    c.color_mode = ColorMode::TrueColorFgBg;
    const AsciiFrame sobel = converted(rgb, 4, 4, c);
    // Every row, the clamped top and bottom ones included.
    VGLYPH_CHECK(glyphs_of(sobel) == U" ||# ||# ||# ||#");

    c.render_mode = RenderMode::Ascii;
    const AsciiFrame ascii = converted(rgb, 4, 4, c);
    VGLYPH_CHECK(ascii.cells.size() == sobel.cells.size());
    for (std::size_t i = 0; i < sobel.cells.size() && i < ascii.cells.size(); ++i) {
        VGLYPH_CHECK(same(sobel.cells[i].fg, ascii.cells[i].fg));
        VGLYPH_CHECK(same(sobel.cells[i].bg, ascii.cells[i].bg));
    }
}

VGLYPH_TEST(sobel_horizontal_step_draws_dashes) {
    const Bytes rgb = gray_rows(4, {0, 0, 255, 255});
    VGLYPH_CHECK(glyphs_of(converted(rgb, 4, 4, sobel_config(4))) == U"    --------####");
    // Brighter above gives the opposite gradient and the same stroke.
    const Bytes flipped = gray_rows(4, {255, 255, 0, 0});
    VGLYPH_CHECK(glyphs_of(converted(flipped, 4, 4, sobel_config(4))) == U"####--------    ");
}

VGLYPH_TEST(sobel_diagonal_steps_draw_slashes) {
    // Interior cells only: a 3x3 kernel spreads a diagonal step over 4 diagonals.
    const auto check = [](bool mirrored, char32_t expected) {
        const auto k_of = [mirrored](std::uint32_t x, std::uint32_t y) {
            return (mirrored ? 7 - x : x) + y;
        };
        const Bytes rgb = gray_plane(8, 8, [&](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(k_of(x, y) >= 8 ? 255 : 0);
        });
        const AsciiFrame out = converted(rgb, 8, 8, sobel_config(8));
        for (std::uint32_t y = 1; y < 7; ++y) {
            for (std::uint32_t x = 1; x < 7; ++x) {
                const std::uint32_t k = k_of(x, y);
                const char32_t want = (k >= 6 && k <= 9) ? expected : (k < 6 ? U' ' : U'#');
                VGLYPH_CHECK(out.cells[y * 8 + x].codepoint == want);
            }
        }
    };
    check(false, U'/');
    check(true, U'\\');
}

VGLYPH_TEST(sobel_threshold_is_a_quarter_of_a_full_step) {
    // A vertical step scores exactly its luminance difference: 63/255 < 0.25 < 64/255.
    const auto step = [](std::uint8_t high) {
        const Bytes rgb = gray_plane(4, 1, [high](std::uint32_t x, std::uint32_t) {
            return static_cast<std::uint8_t>(x < 2 ? 0 : high);
        });
        return glyphs_of(converted(rgb, 4, 1, sobel_config(4)));
    };
    VGLYPH_CHECK(step(63) == U"    ");
    VGLYPH_CHECK(step(64) == U" || ");
}

VGLYPH_TEST(sobel_flat_image_matches_ascii) {
    const Bytes rgb = solid(4, 3, Rgb{90, 160, 200});
    ConvertConfig c;
    c.target_cols = 4;
    const AsciiFrame ascii = converted(rgb, 4, 3, c);
    c.render_mode = RenderMode::SobelAscii;
    VGLYPH_CHECK(same(converted(rgb, 4, 3, c), ascii));
}

VGLYPH_TEST(sobel_invert_keeps_the_strokes) {
    const Bytes rgb = gray_rows(4, {0, 0, 255, 255});
    ConvertConfig c = sobel_config(4);
    c.invert = true;
    VGLYPH_CHECK(glyphs_of(converted(rgb, 4, 4, c)) == U"####--------    ");
}

VGLYPH_TEST(output_glyphs_adds_the_strokes_for_sobel_only) {
    ConvertConfig c;
    c.charset = U" #";
    VGLYPH_CHECK(vglyph::output_glyphs(c) == U" #");
    c.render_mode = RenderMode::HalfBlock;
    VGLYPH_CHECK(vglyph::output_glyphs(c) == U" #");
    c.render_mode = RenderMode::SobelAscii;
    VGLYPH_CHECK(vglyph::output_glyphs(c) == U" #|-/\\");
}

VGLYPH_TEST(braille_maps_each_pixel_to_its_dot_bit) {
    // SPEC 4.3: kBits[row][column] of a 2x4 block.
    constexpr unsigned kBits[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    const ConvertConfig c = braille_config(ColorMode::Mono);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 2; ++x) {
            Bytes rgb = solid(8, 4, Rgb{0, 0, 0});
            set_pixel(rgb, 8, 2 + x, y, Rgb{255, 255, 255});  // in the second cell
            const AsciiFrame out = converted(rgb, 8, 4, c);
            VGLYPH_CHECK(out.cols == 4 && out.rows == 1 && out.cells.size() == 4);
            const std::u32string expected{kBraille, kBraille + kBits[y][x], kBraille, kBraille};
            VGLYPH_CHECK(glyphs_of(out) == expected);
        }
    }
}

VGLYPH_TEST(braille_places_cells_row_major) {
    Bytes rgb = solid(8, 8, Rgb{0, 0, 0});
    set_pixel(rgb, 8, 7, 7, Rgb{255, 255, 255});
    const AsciiFrame out = converted(rgb, 8, 8, braille_config(ColorMode::Mono));
    VGLYPH_CHECK(out.cols == 4 && out.rows == 2 && out.cells.size() == 8);
    for (std::size_t i = 0; i < out.cells.size(); ++i) {
        VGLYPH_CHECK(out.cells[i].codepoint == (i == 7 ? kBraille + 0x80 : kBraille));
    }
}

VGLYPH_TEST(braille_raises_a_dot_at_half_luminance) {
    const ConvertConfig c = braille_config(ColorMode::Mono);
    VGLYPH_CHECK(braille_cell(solid(8, 4, Rgb{127, 127, 127}), c).codepoint == kBraille);
    VGLYPH_CHECK(braille_cell(solid(8, 4, Rgb{128, 128, 128}), c).codepoint == kBraille + 0xFF);
}

VGLYPH_TEST(braille_colors_average_the_raised_dots) {
    const Bytes rgb = two_dot_plane();
    constexpr char32_t kTwoDots = kBraille + 0x81;

    const Cell mono = braille_cell(rgb, braille_config(ColorMode::Mono));
    VGLYPH_CHECK(same(mono, Cell{kTwoDots, Rgb{255, 255, 255}, Rgb{1, 2, 3}}));
    const Cell gray = braille_cell(rgb, braille_config(ColorMode::Grayscale));
    VGLYPH_CHECK(same(gray, Cell{kTwoDots, Rgb{227, 227, 227}, Rgb{1, 2, 3}}));
    const Cell fg = braille_cell(rgb, braille_config(ColorMode::TrueColorFg));
    VGLYPH_CHECK(same(fg, Cell{kTwoDots, Rgb{255, 228, 128}, Rgb{1, 2, 3}}));
    const Cell fgbg = braille_cell(rgb, braille_config(ColorMode::TrueColorFgBg));
    VGLYPH_CHECK(same(fgbg, Cell{kTwoDots, Rgb{255, 228, 128}, Rgb{64, 57, 32}}));
}

VGLYPH_TEST(braille_colors_average_the_block_without_dots) {
    const Bytes rgb = two_rows(8, Rgb{40, 0, 0}, Rgb{0, 0, 80});
    Bytes block = rgb;
    block.insert(block.end(), rgb.begin(), rgb.end());  // rows: red, blue, red, blue

    const Cell fg = braille_cell(block, braille_config(ColorMode::TrueColorFg));
    VGLYPH_CHECK(same(fg, Cell{kBraille, Rgb{20, 0, 40}, Rgb{1, 2, 3}}));
    const Cell gray = braille_cell(block, braille_config(ColorMode::Grayscale));
    VGLYPH_CHECK(same(gray, Cell{kBraille, Rgb{7, 7, 7}, Rgb{1, 2, 3}}));
}

VGLYPH_TEST(braille_invert_flips_the_dots) {
    ConvertConfig c = braille_config(ColorMode::Grayscale);
    c.invert = true;
    Bytes rgb = solid(8, 4, Rgb{0, 0, 0});
    set_pixel(rgb, 8, 0, 0, Rgb{255, 255, 255});
    // The seven black pixels are raised, and their inverted luminance is full.
    VGLYPH_CHECK(same(braille_cell(rgb, c), Cell{kBraille + 0xFE, Rgb{255, 255, 255}, Rgb{1, 2, 3}}));
}

VGLYPH_TEST(braille_ignores_the_charset) {
    const Bytes rgb = two_dot_plane();
    ConvertConfig c = braille_config(ColorMode::TrueColorFg);
    const AsciiFrame with_default = converted(rgb, 8, 4, c);
    c.charset = U"ab";
    VGLYPH_CHECK(same(converted(rgb, 8, 4, c), with_default));
}

VGLYPH_TEST(braille_checks_the_buffer_against_two_by_four_cells) {
    const ConvertConfig c = braille_config(ColorMode::Mono);
    const Rgb black{0, 0, 0};
    AsciiFrame out;
    VGLYPH_CHECK(has_code(run(solid(4, 4, black), 4, 4, c, out), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(run(Bytes{}, 8, 0, c, out), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(run(solid(8, 3, black), 8, 3, c, out), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(run(solid(8, 6, black), 8, 6, c, out), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(run(solid(8, 4, black), 8, 8, c, out), ErrorCode::InvalidArgument));
    ConvertConfig rows = c;
    rows.target_rows = 2;
    VGLYPH_CHECK(has_code(run(solid(8, 4, black), 8, 4, rows, out), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(run(solid(8, 4100, black), 8, 4100, c, out), ErrorCode::LimitExceeded));
    VGLYPH_CHECK(out.cells.empty() && out.cols == 0 && out.rows == 0);

    VGLYPH_CHECK(run(solid(8, 4096, black), 8, 4096, c, out).ok());
    VGLYPH_CHECK(out.rows == 1024);
}

VGLYPH_TEST(output_glyphs_of_braille_is_only_a_space) {
    ConvertConfig c;
    c.render_mode = RenderMode::Braille;
    VGLYPH_CHECK(vglyph::output_glyphs(c) == U" ");
}

VGLYPH_TEST_MAIN()
