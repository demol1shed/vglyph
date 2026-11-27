#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "braille.hpp"
#include "checked_math.hpp"
#include "vglyph/convert.hpp"
#include "vglyph/rasterizer.hpp"

namespace vglyph {

namespace {

constexpr std::size_t kBytesPerPixel = 4;

// Linear blend of one channel, rounded; `alpha` is the glyph coverage 0..255.
[[nodiscard]] std::uint8_t blend(std::uint8_t fg, std::uint8_t bg, unsigned alpha) noexcept {
    const unsigned mixed = static_cast<unsigned>(fg) * alpha +
                           static_cast<unsigned>(bg) * (255u - alpha) + 127u;
    return static_cast<std::uint8_t>(mixed / 255u);
}

void put(std::uint8_t* pixel, Rgb color) noexcept {
    pixel[0] = color.r;
    pixel[1] = color.g;
    pixel[2] = color.b;
    pixel[3] = 255;
}

// Fills columns [col_begin, col_end) of rows [row_begin, row_end) of one cell
// with a single color.
void fill_rect(std::uint8_t* cell, std::size_t stride, std::uint32_t col_begin,
               std::uint32_t col_end, std::uint32_t row_begin, std::uint32_t row_end,
               Rgb color) noexcept {
    for (std::uint32_t row = row_begin; row < row_end; ++row) {
        std::uint8_t* pixel = cell + static_cast<std::size_t>(row) * stride +
                              static_cast<std::size_t>(col_begin) * kBytesPerPixel;
        for (std::uint32_t col = col_begin; col < col_end; ++col) {
            put(pixel, color);
            pixel += kBytesPerPixel;
        }
    }
}

void draw_glyph(std::uint8_t* cell, std::size_t stride, std::uint32_t cell_w, std::uint32_t cell_h,
                const std::uint8_t* coverage, const Cell& source) noexcept {
    for (std::uint32_t row = 0; row < cell_h; ++row) {
        std::uint8_t* pixel = cell + static_cast<std::size_t>(row) * stride;
        const std::uint8_t* alpha = coverage + static_cast<std::size_t>(row) * cell_w;
        for (std::uint32_t col = 0; col < cell_w; ++col) {
            const unsigned a = alpha[col];
            if (a == 255) {
                put(pixel, source.fg);
            } else if (a != 0) {
                pixel[0] = blend(source.fg.r, source.bg.r, a);
                pixel[1] = blend(source.fg.g, source.bg.g, a);
                pixel[2] = blend(source.fg.b, source.bg.b, a);
            }
            pixel += kBytesPerPixel;
        }
    }
}

// Draws the raised dots of a Braille cell (SPEC section 7.1): the cell splits
// into 2 x 4 sub-cells, and a dot is its sub-cell inset by a quarter on each side.
void draw_braille(std::uint8_t* cell, std::size_t stride, std::uint32_t cell_w,
                  std::uint32_t cell_h, const Cell& source) noexcept {
    const auto raised = static_cast<unsigned>(source.codepoint - kBrailleBlank);
    for (std::uint32_t row = 0; row < 4; ++row) {
        const std::uint32_t top = row * cell_h / 4;
        const std::uint32_t bottom = (row + 1) * cell_h / 4;
        const std::uint32_t inset_y = (bottom - top) / 4;
        for (std::uint32_t col = 0; col < 2; ++col) {
            if ((raised & kBrailleDotBit[row][col]) == 0) {
                continue;
            }
            const std::uint32_t left = col * cell_w / 2;
            const std::uint32_t right = (col + 1) * cell_w / 2;
            const std::uint32_t inset_x = (right - left) / 4;
            fill_rect(cell, stride, left + inset_x, right - inset_x, top + inset_y,
                      bottom - inset_y, source.fg);
        }
    }
}

}  // namespace

Status rasterize(const AsciiFrame& frame, const GlyphAtlas& atlas, RgbaImage& out) {
    if (frame.cols == 0 || frame.rows == 0) {
        return make_error(ErrorCode::InvalidArgument, "the frame has no cells");
    }
    std::uint64_t cell_count = 0;
    if (!checked_mul(frame.cols, frame.rows, cell_count) || cell_count != frame.cells.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          "the frame's cell count does not match cols * rows");
    }

    const std::uint32_t cell_w = atlas.cell_px_w();
    const std::uint32_t cell_h = atlas.cell_px_h();
    // Both factors are 32-bit, so these products cannot overflow 64 bits.
    const std::uint64_t width = static_cast<std::uint64_t>(frame.cols) * cell_w;
    const std::uint64_t height = static_cast<std::uint64_t>(frame.rows) * cell_h;
    if (width > kMaxSourceDim || height > kMaxSourceDim || width * height > kMaxSourcePixels) {
        return make_error(ErrorCode::LimitExceeded,
                          "the " + std::to_string(width) + "x" + std::to_string(height) +
                              " pixel image is over the size limit (16384 per side, 64 megapixels)");
    }
    const std::size_t stride = static_cast<std::size_t>(width) * kBytesPerPixel;
    const std::size_t bytes = stride * static_cast<std::size_t>(height);
    if (out.rgba.size() != bytes) {
        out.rgba.resize(bytes);
    }
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);

    const std::uint32_t half = cell_h / 2;
    for (std::uint32_t row = 0; row < frame.rows; ++row) {
        for (std::uint32_t col = 0; col < frame.cols; ++col) {
            const Cell& source = frame.cells[static_cast<std::size_t>(row) * frame.cols + col];
            std::uint8_t* cell = out.rgba.data() +
                                 static_cast<std::size_t>(row) * cell_h * stride +
                                 static_cast<std::size_t>(col) * cell_w * kBytesPerPixel;
            if (source.codepoint == kUpperHalfBlock) {
                fill_rect(cell, stride, 0, cell_w, 0, half, source.fg);
                fill_rect(cell, stride, 0, cell_w, half, cell_h, source.bg);
                continue;
            }
            if (is_braille(source.codepoint)) {
                fill_rect(cell, stride, 0, cell_w, 0, cell_h, source.bg);
                draw_braille(cell, stride, cell_w, cell_h, source);
                continue;
            }
            const GlyphAtlas::Lookup glyph = atlas.find(source.codepoint);
            if (!glyph.found) {
                char label[16];
                const int n = std::snprintf(label, sizeof label, "U+%04X",
                                            static_cast<unsigned>(source.codepoint));
                return make_error(ErrorCode::InvalidArgument,
                                  std::string(n > 0 && static_cast<std::size_t>(n) < sizeof label
                                                  ? label
                                                  : "a codepoint") +
                                      " is not in the glyph atlas");
            }
            fill_rect(cell, stride, 0, cell_w, 0, cell_h, source.bg);
            if (glyph.coverage != nullptr) {
                draw_glyph(cell, stride, cell_w, cell_h, glyph.coverage, source);
            }
        }
    }
    return Status::success();
}

}  // namespace vglyph
