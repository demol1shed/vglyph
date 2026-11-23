// The only translation unit that includes stb_truetype.h with its
// implementation. Every stbtt_* call and its return value stays in this file.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "checked_math.hpp"
#include "vglyph/rasterizer.hpp"
#include "vglyph_embedded_font.hpp"

namespace vglyph {

namespace {

constexpr std::size_t kMaxFontBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t kMaxCellPx = 256;               // a sane cell for an 8..64 px font
constexpr std::uint64_t kMaxAtlasBytes = 64u * 1024u * 1024u;
constexpr char32_t kMaxCodepoint = 0x10FFFF;

[[nodiscard]] std::string codepoint_label(char32_t codepoint) {
    char buf[16];
    const int n = std::snprintf(buf, sizeof buf, "U+%04X", static_cast<unsigned>(codepoint));
    return n > 0 && static_cast<std::size_t>(n) < sizeof buf ? std::string(buf) : "a codepoint";
}

}  // namespace

std::span<const std::uint8_t> default_font_data() noexcept {
    return {embedded::kFontData, embedded::kFontSize};
}

Result<GlyphAtlas> GlyphAtlas::create_default(std::uint32_t px_height,
                                              std::u32string_view charset) {
    return create(default_font_data(), px_height, charset);
}

Result<GlyphAtlas> GlyphAtlas::create(std::span<const std::uint8_t> ttf, std::uint32_t px_height,
                                      std::u32string_view charset) {
    if (px_height < kMinFontPx || px_height > kMaxFontPx) {
        return make_error(ErrorCode::InvalidArgument,
                          "font size must be " + std::to_string(kMinFontPx) + ".." +
                              std::to_string(kMaxFontPx) + " pixels");
    }
    if (charset.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the character set is empty");
    }
    if (ttf.size() < 12) {
        return make_error(ErrorCode::FontError, "the font file is not a TrueType font");
    }
    if (ttf.size() > kMaxFontBytes) {
        return make_error(ErrorCode::LimitExceeded, "the font file is larger than 64 MiB");
    }

    stbtt_fontinfo font{};
    const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    if (offset < 0 || stbtt_InitFont(&font, ttf.data(), offset) == 0) {
        return make_error(ErrorCode::FontError,
                          "cannot load the font (only TrueType outlines are supported)");
    }

    // Sorted unique codepoints; the half block is drawn without the font.
    std::vector<char32_t> codepoints(charset.begin(), charset.end());
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    codepoints.erase(std::remove(codepoints.begin(), codepoints.end(), kUpperHalfBlock),
                     codepoints.end());

    // Pass 1: every glyph must exist; the widest advance sets the cell width.
    const float scale = stbtt_ScaleForMappingEmToPixels(&font, static_cast<float>(px_height));
    int advance = 0;
    int bearing = 0;
    stbtt_GetCodepointHMetrics(&font, ' ', &advance, &bearing);
    int max_advance = advance;
    std::vector<int> glyphs;
    glyphs.reserve(codepoints.size());
    for (const char32_t codepoint : codepoints) {
        const int glyph = codepoint > kMaxCodepoint
                              ? 0
                              : stbtt_FindGlyphIndex(&font, static_cast<int>(codepoint));
        if (glyph == 0) {
            return make_error(ErrorCode::FontError,
                              "the font has no glyph for " + codepoint_label(codepoint));
        }
        stbtt_GetGlyphHMetrics(&font, glyph, &advance, &bearing);
        max_advance = std::max(max_advance, advance);
        glyphs.push_back(glyph);
    }

    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
    const float width_px = std::ceil(static_cast<float>(max_advance) * scale);
    const float height_px = std::ceil(static_cast<float>(ascent - descent + line_gap) * scale);
    const float max_cell = static_cast<float>(kMaxCellPx);
    if (!(width_px >= 1.0f && width_px <= max_cell && height_px >= 1.0f && height_px <= max_cell)) {
        return make_error(ErrorCode::FontError, "the font metrics are out of range");
    }

    GlyphAtlas atlas;
    atlas.cell_w_ = static_cast<std::uint32_t>(width_px);
    atlas.cell_h_ = static_cast<std::uint32_t>(height_px);
    const std::size_t tile_bytes = static_cast<std::size_t>(atlas.cell_w_) * atlas.cell_h_;
    std::uint64_t atlas_bytes = 0;
    if (!checked_mul(tile_bytes, glyphs.size(), atlas_bytes) || atlas_bytes > kMaxAtlasBytes) {
        return make_error(ErrorCode::LimitExceeded, "the glyph atlas would exceed 64 MiB");
    }
    atlas.entries_.reserve(glyphs.size());
    atlas.coverage_.reserve(static_cast<std::size_t>(atlas_bytes));

    // Pass 2: rasterize each glyph into a cell-sized tile, clipped to the cell.
    const int baseline = static_cast<int>(std::lround(static_cast<float>(ascent) * scale));
    const int cell_w = static_cast<int>(atlas.cell_w_);
    const int cell_h = static_cast<int>(atlas.cell_h_);
    std::vector<std::uint8_t> bitmap;
    std::vector<std::uint8_t> tile(tile_bytes);
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetGlyphBitmapBox(&font, glyphs[i], scale, scale, &x0, &y0, &x1, &y1);
        const int w = x1 - x0;
        const int h = y1 - y0;
        // A box far larger than a cell means a corrupt font; it would be clipped
        // anyway, so refuse before allocating its bitmap.
        if (w < 0 || h < 0 || w > 4 * cell_w || h > 4 * cell_h) {
            return make_error(ErrorCode::FontError,
                              "the glyph for " + codepoint_label(codepoints[i]) +
                                  " is out of range");
        }

        std::fill(tile.begin(), tile.end(), std::uint8_t{0});
        bool blank = true;
        if (w > 0 && h > 0) {
            bitmap.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
            stbtt_MakeGlyphBitmap(&font, bitmap.data(), w, h, w, scale, scale, glyphs[i]);
            for (int row = 0; row < h; ++row) {
                const int dy = baseline + y0 + row;
                if (dy < 0 || dy >= cell_h) {
                    continue;
                }
                for (int col = 0; col < w; ++col) {
                    const int dx = x0 + col;
                    if (dx < 0 || dx >= cell_w) {
                        continue;
                    }
                    const std::uint8_t value =
                        bitmap[static_cast<std::size_t>(row) * static_cast<std::size_t>(w) +
                               static_cast<std::size_t>(col)];
                    tile[static_cast<std::size_t>(dy) * static_cast<std::size_t>(cell_w) +
                         static_cast<std::size_t>(dx)] = value;
                    blank = blank && value == 0;
                }
            }
        }

        Entry entry;
        entry.codepoint = codepoints[i];
        entry.blank = blank;
        if (!blank) {
            entry.offset = atlas.coverage_.size();
            atlas.coverage_.insert(atlas.coverage_.end(), tile.begin(), tile.end());
        }
        atlas.entries_.push_back(entry);
    }
    return atlas;
}

GlyphAtlas::Lookup GlyphAtlas::find(char32_t codepoint) const noexcept {
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), codepoint,
        [](const Entry& entry, char32_t value) { return entry.codepoint < value; });
    if (it == entries_.end() || it->codepoint != codepoint) {
        return {};
    }
    return {true, it->blank ? nullptr : coverage_.data() + it->offset};
}

}  // namespace vglyph
