#include "vglyph/convert.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <string_view>

#include "checked_math.hpp"

namespace vglyph {

namespace {

// False for NaN, since every comparison with NaN is false.
[[nodiscard]] bool in_range(float v, float lo, float hi) noexcept { return v >= lo && v <= hi; }

// Charset glyphs end up in .ans files and on the terminal, so C0/C1 controls
// (including ESC) must never get through. Surrogates and values past U+10FFFF
// are not Unicode scalar values.
[[nodiscard]] bool is_printable_codepoint(char32_t c) noexcept {
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) {
        return false;
    }
    if (c >= 0xD800 && c <= 0xDFFF) {
        return false;
    }
    return c <= 0x10FFFF;
}

[[nodiscard]] Error invalid(const char* field, const char* range) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(field) + " must be in " + range);
}

// Preprocessed value (SPEC section 4.2) for every possible 8-bit channel value.
// Built once per convert() call on the stack, so pow() runs 256 times, not per pixel.
using Lut = std::array<float, 256>;

[[nodiscard]] Lut build_lut(const ConvertConfig& config) {
    Lut lut{};
    for (std::size_t i = 0; i < lut.size(); ++i) {
        const float c = static_cast<float>(i) / 255.0f;
        const float gamma = std::pow(c, config.gamma);
        const float adjusted = (gamma - 0.5f) * config.contrast + 0.5f + config.brightness;
        lut[i] = std::clamp(adjusted, 0.0f, 1.0f);
    }
    return lut;
}

[[nodiscard]] std::uint8_t to_byte(float unit) noexcept {
    return static_cast<std::uint8_t>(std::lround(unit * 255.0f));
}

// Luminance Y' in 0..1 (BT.709, then optional inversion) of the pixel at `px`.
[[nodiscard]] float luminance(const Lut& lut, const std::uint8_t* px, bool invert) noexcept {
    const float y = 0.2126f * lut[px[0]] + 0.7152f * lut[px[1]] + 0.0722f * lut[px[2]];
    const float clamped = std::min(y, 1.0f);  // the coefficients can sum to just over 1 in float
    return invert ? 1.0f - clamped : clamped;
}

[[nodiscard]] Rgb true_color(const Lut& lut, const std::uint8_t* px) noexcept {
    return Rgb{to_byte(lut[px[0]]), to_byte(lut[px[1]]), to_byte(lut[px[2]])};
}

[[nodiscard]] Rgb gray(float y) noexcept {
    const std::uint8_t g = to_byte(y);
    return Rgb{g, g, g};
}

constexpr Rgb kWhite{255, 255, 255};

// U+2580 UPPER HALF BLOCK. Written numerically so the source stays ASCII
// (MSVC reads sources in the ANSI code page without /utf-8).
constexpr char32_t kUpperHalfBlock = 0x2580;

// Darkened background of TrueColorFgBg: round(0.25 * c), in integers.
[[nodiscard]] Rgb quarter(Rgb c) noexcept {
    const auto q = [](std::uint8_t v) { return static_cast<std::uint8_t>((v + 2) / 4); };
    return Rgb{q(c.r), q(c.g), q(c.b)};
}

void fill_ascii(const std::uint8_t* rgb, const ConvertConfig& config, const Lut& lut,
                AsciiFrame& out) noexcept {
    const std::size_t levels = config.charset.size();
    const float levels_f = static_cast<float>(levels);
    Cell* cell = out.cells.data();
    for (std::size_t i = 0, n = out.cells.size(); i < n; ++i, rgb += 3, ++cell) {
        const float y = luminance(lut, rgb, config.invert);
        const std::size_t idx = std::min(levels - 1, static_cast<std::size_t>(y * levels_f));
        cell->codepoint = config.charset[idx];
        cell->bg = config.bg_color;
        switch (config.color_mode) {
            case ColorMode::Mono:
                cell->fg = kWhite;
                break;
            case ColorMode::Grayscale:
                cell->fg = gray(y);
                break;
            case ColorMode::TrueColorFg:
                cell->fg = true_color(lut, rgb);
                break;
            case ColorMode::TrueColorFgBg:
                cell->fg = true_color(lut, rgb);
                cell->bg = quarter(cell->fg);
                break;
        }
    }
}

// Edge strokes of SobelAscii (SPEC section 4.3).
constexpr std::u32string_view kStrokes = U"|-/\\";
// Smallest |G| / 4 that counts as an edge; a black-to-white step scores 1.0.
constexpr float kSobelThreshold = 0.25f;

// Stroke for a gradient: it runs perpendicular to the gradient angle (y grows downward).
[[nodiscard]] char32_t stroke(float gx, float gy) noexcept {
    float degrees = std::atan2(gy, gx) * (180.0f / std::numbers::pi_v<float>);
    if (degrees < 0.0f) {
        degrees += 180.0f;
    }
    if (degrees >= 22.5f && degrees < 67.5f) {
        return U'/';
    }
    if (degrees >= 67.5f && degrees < 112.5f) {
        return U'-';
    }
    if (degrees >= 112.5f && degrees < 157.5f) {
        return U'\\';
    }
    return U'|';  // around 0 and 180 degrees
}

// The Ascii frame, with the glyph of every edge cell replaced by a stroke.
void fill_sobel(const std::uint8_t* rgb, const ConvertConfig& config, const Lut& lut,
                AsciiFrame& out) noexcept {
    fill_ascii(rgb, config, lut, out);

    const std::size_t cols = out.cols;
    const std::size_t rows = out.rows;
    const auto y_at = [&](std::size_t x, std::size_t y) {
        return luminance(lut, rgb + (y * cols + x) * 3, config.invert);
    };
    Cell* cell = out.cells.data();
    for (std::size_t y = 0; y < rows; ++y) {
        // Neighbors outside the plane repeat the nearest pixel inside it.
        const std::size_t up = y == 0 ? y : y - 1;
        const std::size_t down = y + 1 == rows ? y : y + 1;
        for (std::size_t x = 0; x < cols; ++x, ++cell) {
            const std::size_t left = x == 0 ? x : x - 1;
            const std::size_t right = x + 1 == cols ? x : x + 1;
            const float nw = y_at(left, up);
            const float ne = y_at(right, up);
            const float sw = y_at(left, down);
            const float se = y_at(right, down);
            const float gx = (ne - nw) + 2.0f * (y_at(right, y) - y_at(left, y)) + (se - sw);
            const float gy = (sw - nw) + 2.0f * (y_at(x, down) - y_at(x, up)) + (se - ne);
            if (std::sqrt(gx * gx + gy * gy) / 4.0f >= kSobelThreshold) {
                cell->codepoint = stroke(gx, gy);
            }
        }
    }
}

// Color of one half-block pixel: `fg` takes the top pixel and `bg` the bottom one.
[[nodiscard]] Rgb half_color(const std::uint8_t* px, const ConvertConfig& config,
                             const Lut& lut) noexcept {
    switch (config.color_mode) {
        case ColorMode::Mono:
            return luminance(lut, px, config.invert) >= 0.5f ? kWhite : config.bg_color;
        case ColorMode::Grayscale:
            return gray(luminance(lut, px, config.invert));
        case ColorMode::TrueColorFg:
        case ColorMode::TrueColorFgBg:
            return true_color(lut, px);
    }
    return config.bg_color;  // unreachable for valid enum values
}

void fill_half_block(const std::uint8_t* rgb, const ConvertConfig& config, const Lut& lut,
                     AsciiFrame& out) noexcept {
    const std::size_t row_bytes = static_cast<std::size_t>(out.cols) * 3;
    Cell* cell = out.cells.data();
    for (std::uint32_t row = 0; row < out.rows; ++row) {
        const std::uint8_t* top = rgb + static_cast<std::size_t>(row) * 2 * row_bytes;
        const std::uint8_t* bottom = top + row_bytes;
        for (std::uint32_t col = 0; col < out.cols; ++col, top += 3, bottom += 3, ++cell) {
            cell->codepoint = kUpperHalfBlock;
            cell->fg = half_color(top, config, lut);
            cell->bg = half_color(bottom, config, lut);
        }
    }
}

// Sampling-plane pixels per grid cell on each axis (SPEC section 4.1).
struct CellPixels {
    std::uint32_t w = 1;
    std::uint32_t h = 1;
};

[[nodiscard]] CellPixels cell_pixels(RenderMode mode) noexcept {
    switch (mode) {
        case RenderMode::Ascii:
        case RenderMode::SobelAscii:
            break;
        case RenderMode::HalfBlock:
            return {1, 2};
    }
    return {};
}

}  // namespace

Status validate(const ConvertConfig& config) {
    if (config.target_cols < 4 || config.target_cols > kMaxGridCols) {
        return invalid("target_cols", "4..2048");
    }
    if (config.target_rows > kMaxGridRows) {
        return invalid("target_rows", "0..1024 (0 = auto)");
    }
    if (!in_range(config.cell_aspect, 0.1f, 4.0f)) {
        return invalid("cell_aspect", "0.1..4.0");
    }
    if (!in_range(config.brightness, -1.0f, 1.0f)) {
        return invalid("brightness", "-1.0..1.0");
    }
    if (!in_range(config.contrast, 0.0f, 4.0f)) {
        return invalid("contrast", "0.0..4.0");
    }
    if (!in_range(config.gamma, 0.1f, 4.0f)) {
        return invalid("gamma", "0.1..4.0");
    }
    if (config.charset.size() < 2) {
        return make_error(ErrorCode::InvalidArgument, "charset must have at least 2 codepoints");
    }
    for (const char32_t c : config.charset) {
        if (!is_printable_codepoint(c)) {
            return make_error(ErrorCode::InvalidArgument,
                              "charset contains a control, surrogate or out-of-range codepoint");
        }
    }
    return Status::success();
}

std::u32string output_glyphs(const ConvertConfig& config) {
    std::u32string glyphs = config.charset;
    if (config.render_mode == RenderMode::SobelAscii) {
        glyphs += kStrokes;
    }
    return glyphs;
}

Result<GridSize> compute_grid(std::uint32_t src_w, std::uint32_t src_h,
                              const ConvertConfig& config) {
    if (const Status s = validate(config); !s.ok()) {
        return s.error();
    }
    if (src_w == 0 || src_h == 0 || src_w > kMaxSourceDim || src_h > kMaxSourceDim) {
        return make_error(ErrorCode::LimitExceeded,
                          "source dimensions must be 1.." + std::to_string(kMaxSourceDim) +
                              " pixels per axis, got " + std::to_string(src_w) + "x" +
                              std::to_string(src_h));
    }
    std::uint64_t src_pixels = 0;
    if (!checked_mul(src_w, src_h, src_pixels) || src_pixels > kMaxSourcePixels) {
        return make_error(ErrorCode::LimitExceeded,
                          "source exceeds " + std::to_string(kMaxSourcePixels) + " pixels");
    }

    std::uint32_t rows = config.target_rows;
    if (rows == 0) {
        // Rounded in double, then compared before narrowing: with the caps above the
        // value is at most ~1.4e8, so the comparison cannot overflow.
        const double derived = std::round(static_cast<double>(config.target_cols) *
                                          static_cast<double>(src_h) / static_cast<double>(src_w) *
                                          static_cast<double>(config.cell_aspect));
        if (derived > static_cast<double>(kMaxGridRows)) {
            return make_error(ErrorCode::LimitExceeded,
                              "derived grid height exceeds " + std::to_string(kMaxGridRows) +
                                  " rows; lower target_cols or set target_rows");
        }
        rows = derived < 1.0 ? 1U : static_cast<std::uint32_t>(derived);
    }

    GridSize grid;
    grid.cols = config.target_cols;
    grid.rows = rows;
    const CellPixels px = cell_pixels(config.render_mode);
    grid.sample_w = grid.cols * px.w;
    grid.sample_h = grid.rows * px.h;

    std::uint64_t sample_pixels = 0;
    if (!checked_mul(grid.sample_w, grid.sample_h, sample_pixels) ||
        !checked_mul(sample_pixels, 3, grid.sample_bytes)) {
        return make_error(ErrorCode::LimitExceeded, "sampling buffer size overflows");
    }
    return grid;
}

Status convert(std::span<const std::uint8_t> rgb24, std::uint32_t width, std::uint32_t height,
               const ConvertConfig& config, AsciiFrame& out) {
    if (const Status s = validate(config); !s.ok()) {
        return s;
    }
    const CellPixels px = cell_pixels(config.render_mode);
    const std::uint32_t sample_w = config.target_cols * px.w;  // validated: cannot overflow
    if (width != sample_w) {
        return make_error(ErrorCode::InvalidArgument,
                          "buffer width " + std::to_string(width) + " must be " +
                              std::to_string(sample_w) + " (target_cols x " +
                              std::to_string(px.w) + ")");
    }
    if (height == 0 || height % px.h != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "buffer height must be a non-zero multiple of " + std::to_string(px.h));
    }
    const std::uint32_t rows = height / px.h;
    if (rows > kMaxGridRows) {
        return make_error(ErrorCode::LimitExceeded,
                          "grid height exceeds " + std::to_string(kMaxGridRows) + " rows");
    }
    if (config.target_rows != 0 && config.target_rows != rows) {
        return make_error(ErrorCode::InvalidArgument,
                          "buffer holds " + std::to_string(rows) + " rows but target_rows is " +
                              std::to_string(config.target_rows));
    }
    std::uint64_t pixels = 0;
    std::uint64_t bytes = 0;
    if (!checked_mul(width, height, pixels) || !checked_mul(pixels, 3, bytes)) {
        return make_error(ErrorCode::LimitExceeded, "buffer size overflows");
    }
    if (rgb24.size() != bytes) {
        return make_error(ErrorCode::InvalidArgument,
                          "buffer holds " + std::to_string(rgb24.size()) + " bytes, expected " +
                              std::to_string(bytes));
    }

    // Everything is validated: from here on `out` is modified.
    const std::size_t cell_count = static_cast<std::size_t>(config.target_cols) * rows;
    if (out.cells.size() != cell_count) {
        out.cells.resize(cell_count);
    }
    out.cols = config.target_cols;
    out.rows = rows;

    const Lut lut = build_lut(config);
    switch (config.render_mode) {
        case RenderMode::Ascii:
            fill_ascii(rgb24.data(), config, lut, out);
            break;
        case RenderMode::HalfBlock:
            fill_half_block(rgb24.data(), config, lut, out);
            break;
        case RenderMode::SobelAscii:
            fill_sobel(rgb24.data(), config, lut, out);
            break;
    }
    return Status::success();
}

}  // namespace vglyph
