#pragma once

// Glyph atlas and RGBA rasterizer for visual export (SPEC section 7.1).
// The header exposes no stb_truetype types.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "vglyph/error.hpp"
#include "vglyph/frame.hpp"

namespace vglyph {

// Upper half block: drawn as two solid halves, never through the font.
inline constexpr char32_t kUpperHalfBlock = 0x2580;

// Font pixel sizes accepted by GlyphAtlas::create (SPEC 7.1).
inline constexpr std::uint32_t kMinFontPx = 8;
inline constexpr std::uint32_t kMaxFontPx = 64;

// Tightly packed RGBA32, alpha always 255. The caller owns it and passes the
// same object to every rasterize() call: `rgba` is resized only when the
// output size changes.
struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  // length width * height * 4
};

// The bytes of the embedded default font (JetBrains Mono Regular, SIL OFL).
[[nodiscard]] std::span<const std::uint8_t> default_font_data() noexcept;

// Coverage bitmaps of every glyph of a character ramp, rasterized once for one
// font and pixel size. The atlas copies what it needs, so the font bytes may
// be freed after create() returns.
class GlyphAtlas {
public:
    struct Lookup {
        bool found = false;
        // cell_px_w() * cell_px_h() coverage bytes (0..255), or null for a
        // blank glyph such as the space.
        const std::uint8_t* coverage = nullptr;
    };

    // `px_height` is the font size in pixels per em (8..64, InvalidArgument
    // otherwise). Fails with FontError if the data is not a TrueType font or a
    // codepoint of `charset` has no glyph (the codepoint is named in the
    // detail; there is no fallback glyph). U+2580 and U+2800..U+28FF need no glyph.
    [[nodiscard]] static Result<GlyphAtlas> create(std::span<const std::uint8_t> ttf,
                                                   std::uint32_t px_height,
                                                   std::u32string_view charset);

    // Same, with the embedded default font.
    [[nodiscard]] static Result<GlyphAtlas> create_default(std::uint32_t px_height,
                                                           std::u32string_view charset);

    GlyphAtlas(GlyphAtlas&&) noexcept = default;
    GlyphAtlas& operator=(GlyphAtlas&&) noexcept = default;
    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;
    ~GlyphAtlas() = default;

    [[nodiscard]] std::uint32_t cell_px_w() const noexcept { return cell_w_; }
    [[nodiscard]] std::uint32_t cell_px_h() const noexcept { return cell_h_; }
    // Cell width / height, the value for ConvertConfig::cell_aspect.
    [[nodiscard]] float cell_aspect() const noexcept {
        return static_cast<float>(cell_w_) / static_cast<float>(cell_h_);
    }

    // Allocation-free; U+2580 and U+2800..U+28FF are never atlas glyphs.
    [[nodiscard]] Lookup find(char32_t codepoint) const noexcept;

private:
    struct Entry {
        char32_t codepoint = 0;
        std::size_t offset = 0;  // into coverage_; unused when blank
        bool blank = false;
    };

    GlyphAtlas() = default;

    std::uint32_t cell_w_ = 0;
    std::uint32_t cell_h_ = 0;
    std::vector<Entry> entries_;          // sorted by codepoint
    std::vector<std::uint8_t> coverage_;  // one cell-sized tile per non-blank glyph
};

// Draws `frame` into `out` (SPEC 7.1): each cell is filled with `bg`, U+2580
// gets `fg` on its top half (rows below cell_px_h / 2) and `bg` below, a Braille
// codepoint gets one solid `fg` rectangle per raised dot, and every other glyph
// is its coverage bitmap blended from `bg` to `fg`.
//
// The output is cols * cell_px_w by rows * cell_px_h pixels. A canvas over
// kMaxSourceDim per axis or kMaxSourcePixels fails with LimitExceeded before
// any allocation. A codepoint that is not in the atlas fails with
// InvalidArgument. On any error `out`'s pixels are unspecified. On success
// `out.rgba` is resized only if the size changed, so a loop that passes the
// same image does not allocate.
[[nodiscard]] Status rasterize(const AsciiFrame& frame, const GlyphAtlas& atlas, RgbaImage& out);

}  // namespace vglyph
