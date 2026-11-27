#pragma once

// Conversion configuration, grid sizing and the pixel-to-cell converter
// (SPEC sections 3.3, 4, 5.3).

#include <cstdint>
#include <span>
#include <string>

#include "vglyph/error.hpp"
#include "vglyph/frame.hpp"

namespace vglyph {

// Safety caps (SPEC section 5.3).
inline constexpr std::uint32_t kMaxGridCols = 2048;
inline constexpr std::uint32_t kMaxGridRows = 1024;
inline constexpr std::uint32_t kMaxSourceDim = 16384;
inline constexpr std::uint64_t kMaxSourcePixels = 67'108'864;  // 64 megapixels

enum class RenderMode { Ascii, HalfBlock, SobelAscii, Braille };
enum class ColorMode { Mono, Grayscale, TrueColorFg, TrueColorFgBg };
enum class DitherMode { None };

struct ConvertConfig {
    std::uint32_t target_cols = 100;  // 4..2048
    std::uint32_t target_rows = 0;    // 0 = derive from source aspect ratio
    float cell_aspect = 0.5f;         // cell width / cell height, 0.1..4.0
    RenderMode render_mode = RenderMode::Ascii;
    ColorMode color_mode = ColorMode::TrueColorFg;
    DitherMode dither_mode = DitherMode::None;
    std::u32string charset = U" .:-=+*#%@";  // dark to bright, at least 2
    Rgb bg_color{0, 0, 0};
    float brightness = 0.0f;  // -1.0..1.0
    float contrast = 1.0f;    // 0.0..4.0
    float gamma = 1.0f;       // 0.1..4.0
    bool invert = false;
};

// Rejects out-of-range values (including NaN) with InvalidArgument.
[[nodiscard]] Status validate(const ConvertConfig& config);

// Output grid and the RGB24 sampling buffer the converter expects.
struct GridSize {
    std::uint32_t cols = 0;
    std::uint32_t rows = 0;
    std::uint32_t sample_w = 0;    // pixels: cols, or 2 * cols (Braille)
    std::uint32_t sample_h = 0;    // pixels: rows, 2 * rows (HalfBlock), 4 * rows (Braille)
    std::uint64_t sample_bytes = 0;  // sample_w * sample_h * 3, overflow-checked
};

// The glyphs a glyph atlas has to cover for the render mode. Braille dots are
// drawn without the font, so that mode only asks for a space.
[[nodiscard]] std::u32string output_glyphs(const ConvertConfig& config);

// Computes the grid for a source of src_w x src_h pixels. Never crops or
// shrinks: a grid over the caps fails with LimitExceeded.
[[nodiscard]] Result<GridSize> compute_grid(std::uint32_t src_w, std::uint32_t src_h,
                                            const ConvertConfig& config);

// Converts a tightly packed RGB24 buffer, already scaled to the sampling grid
// from compute_grid(), into `out` (SPEC sections 4.2, 4.3).
//
// `width` and `height` are the grid size times the mode's pixels per cell: 1x1
// (Ascii, SobelAscii), 1x2 (HalfBlock) or 2x4 (Braille). `width` must match
// config.target_cols, `height` must be a whole number of rows, and a non-zero
// config.target_rows must match that row count.
//
// On any error `out` is left untouched. On success `out.cells` is resized
// only if the grid size changed, so repeated calls reuse one buffer and do
// not allocate. `pts_us` and `duration_us` are the caller's to set.
[[nodiscard]] Status convert(std::span<const std::uint8_t> rgb24, std::uint32_t width,
                             std::uint32_t height, const ConvertConfig& config, AsciiFrame& out);

}  // namespace vglyph
