#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_framework.hpp"
#include "vglyph/rasterizer.hpp"

using namespace vglyph;

// ---- Allocation counter ----------------------------------------------------
// Same technique as tests/player_test.cpp: the global allocation functions are
// replaced for this binary and only counted while g_counting is set.

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

void* counted_malloc(std::size_t size) noexcept {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size == 0 ? 1 : size);
}

void* counted_new(std::size_t size) {
    void* p = counted_malloc(size);
    if (p == nullptr) {
        std::abort();  // no exceptions: an unrecoverable out-of-memory
    }
    return p;
}

}  // namespace

void* operator new(std::size_t size) { return counted_new(size); }
void* operator new[](std::size_t size) { return counted_new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_malloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return counted_malloc(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kRed{200, 30, 30};
constexpr Rgb kBlue{20, 40, 220};
constexpr std::uint32_t kPx = 16;

[[nodiscard]] AsciiFrame make_frame(std::uint32_t cols, std::uint32_t rows, char32_t codepoint,
                                    Rgb fg, Rgb bg) {
    AsciiFrame frame;
    frame.cols = cols;
    frame.rows = rows;
    frame.cells.assign(static_cast<std::size_t>(cols) * rows, Cell{codepoint, fg, bg});
    return frame;
}

[[nodiscard]] GlyphAtlas make_atlas(std::u32string_view charset, std::uint32_t px = kPx) {
    Result<GlyphAtlas> atlas = GlyphAtlas::create_default(px, charset);
    if (atlas.value_if() == nullptr) {
        // The tests below check the same call; stop the run instead of dereferencing null.
        vglyph_test::out("  cannot build the atlas: %s\n", atlas.error_if()->detail.c_str());
        ++vglyph_test::failure_count();
        std::abort();
    }
    return std::move(*atlas.value_if());
}

[[nodiscard]] Rgb pixel(const RgbaImage& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return Rgb{image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]};
}

[[nodiscard]] bool same(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

}  // namespace

VGLYPH_TEST(atlas_metrics_of_the_default_font) {
    const GlyphAtlas atlas = make_atlas(U" .:-=+*#%@");
    // JetBrains Mono: advance 600 and ascent-descent 1320 per 1000 units, at 16 px per em.
    VGLYPH_CHECK(atlas.cell_px_w() == 10);
    VGLYPH_CHECK(atlas.cell_px_h() == 22);
    VGLYPH_CHECK(atlas.cell_aspect() > 0.45f && atlas.cell_aspect() < 0.46f);
}

VGLYPH_TEST(atlas_finds_ramp_glyphs_and_marks_blanks) {
    const GlyphAtlas atlas = make_atlas(U" @");
    const GlyphAtlas::Lookup space = atlas.find(U' ');
    const GlyphAtlas::Lookup at = atlas.find(U'@');
    VGLYPH_CHECK(space.found && space.coverage == nullptr);
    VGLYPH_CHECK(at.found && at.coverage != nullptr);
    VGLYPH_CHECK(!atlas.find(U'A').found);
    VGLYPH_CHECK(!atlas.find(kUpperHalfBlock).found);
}

VGLYPH_TEST(atlas_ignores_duplicates_and_the_half_block) {
    const Result<GlyphAtlas> atlas = GlyphAtlas::create_default(kPx, U"@@ @");
    VGLYPH_CHECK(atlas.value_if() != nullptr);
    const Result<GlyphAtlas> block_only = GlyphAtlas::create_default(kPx, std::u32string(1, kUpperHalfBlock));
    VGLYPH_CHECK(block_only.value_if() != nullptr);
}

VGLYPH_TEST(atlas_rejects_a_codepoint_without_a_glyph) {
    const Result<GlyphAtlas> atlas = GlyphAtlas::create_default(kPx, std::u32string{U' ', U'.', char32_t{0x4E00}});
    VGLYPH_CHECK(atlas.error_if() != nullptr);
    if (atlas.error_if() != nullptr) {
        VGLYPH_CHECK(atlas.error_if()->code == ErrorCode::FontError);
        VGLYPH_CHECK(atlas.error_if()->detail.find("U+4E00") != std::string::npos);
    }
}

VGLYPH_TEST(atlas_rejects_bad_font_data) {
    const std::vector<std::uint8_t> garbage(4096, 0x5A);
    const Result<GlyphAtlas> junk = GlyphAtlas::create(garbage, kPx, U" .");
    VGLYPH_CHECK(junk.error_if() != nullptr && junk.error_if()->code == ErrorCode::FontError);

    const std::vector<std::uint8_t> tiny(4, 0);
    const Result<GlyphAtlas> small = GlyphAtlas::create(tiny, kPx, U" .");
    VGLYPH_CHECK(small.error_if() != nullptr && small.error_if()->code == ErrorCode::FontError);

    const Result<GlyphAtlas> none = GlyphAtlas::create({}, kPx, U" .");
    VGLYPH_CHECK(none.error_if() != nullptr && none.error_if()->code == ErrorCode::FontError);
}

VGLYPH_TEST(atlas_rejects_bad_size_and_empty_charset) {
    for (const std::uint32_t px : {kMinFontPx - 1, kMaxFontPx + 1, std::uint32_t{0}}) {
        const Result<GlyphAtlas> atlas = GlyphAtlas::create_default(px, U" .");
        VGLYPH_CHECK(atlas.error_if() != nullptr &&
                     atlas.error_if()->code == ErrorCode::InvalidArgument);
    }
    VGLYPH_CHECK(GlyphAtlas::create_default(kMinFontPx, U" .").value_if() != nullptr);
    VGLYPH_CHECK(GlyphAtlas::create_default(kMaxFontPx, U" .").value_if() != nullptr);
    const Result<GlyphAtlas> empty = GlyphAtlas::create_default(kPx, U"");
    VGLYPH_CHECK(empty.error_if() != nullptr &&
                 empty.error_if()->code == ErrorCode::InvalidArgument);
}

VGLYPH_TEST(atlas_loads_a_font_from_caller_owned_bytes) {
    // The custom-font path: bytes the caller read from a file, not the embedded array.
    const std::span<const std::uint8_t> embedded = default_font_data();
    VGLYPH_CHECK(embedded.size() > 1000);
    std::vector<std::uint8_t> copy(embedded.begin(), embedded.end());
    Result<GlyphAtlas> custom = GlyphAtlas::create(copy, kPx, U" .:-=+*#%@");
    VGLYPH_CHECK(custom.value_if() != nullptr);
    // The atlas keeps no reference to the bytes.
    copy.assign(copy.size(), 0);
    const GlyphAtlas reference = make_atlas(U" .:-=+*#%@");
    if (custom.value_if() != nullptr) {
        VGLYPH_CHECK(custom.value_if()->cell_px_w() == reference.cell_px_w());
        VGLYPH_CHECK(custom.value_if()->cell_px_h() == reference.cell_px_h());
        VGLYPH_CHECK(custom.value_if()->find(U'#').found);
    }
}

VGLYPH_TEST(rasterize_fills_a_blank_cell_with_the_background) {
    const GlyphAtlas atlas = make_atlas(U" @");
    const AsciiFrame frame = make_frame(2, 1, U' ', kWhite, kRed);
    RgbaImage image;
    VGLYPH_CHECK(rasterize(frame, atlas, image).ok());
    VGLYPH_CHECK(image.width == 2 * atlas.cell_px_w());
    VGLYPH_CHECK(image.height == atlas.cell_px_h());
    VGLYPH_CHECK(image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4);
    bool all_background = true;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        all_background = all_background && image.rgba[i] == kRed.r && image.rgba[i + 1] == kRed.g &&
                         image.rgba[i + 2] == kRed.b && image.rgba[i + 3] == 255;
    }
    VGLYPH_CHECK(all_background);
}

VGLYPH_TEST(rasterize_splits_the_half_block_at_half_the_cell_height) {
    const GlyphAtlas atlas = make_atlas(U" @");
    const AsciiFrame frame = make_frame(1, 1, kUpperHalfBlock, kRed, kBlue);
    RgbaImage image;
    VGLYPH_CHECK(rasterize(frame, atlas, image).ok());
    const std::uint32_t split = atlas.cell_px_h() / 2;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            VGLYPH_CHECK(same(pixel(image, x, y), y < split ? kRed : kBlue));
        }
    }
}

VGLYPH_TEST(rasterize_blends_a_glyph_between_background_and_foreground) {
    const GlyphAtlas atlas = make_atlas(U" @");
    const AsciiFrame frame = make_frame(1, 1, U'@', kWhite, kBlack);
    RgbaImage image;
    VGLYPH_CHECK(rasterize(frame, atlas, image).ok());
    std::size_t solid = 0;
    std::size_t partial = 0;
    bool gray = true;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const Rgb p = pixel(image, x, y);
            gray = gray && p.r == p.g && p.g == p.b;  // white over black stays gray
            solid += p.r == 255 ? 1 : 0;
            partial += p.r != 0 && p.r != 255 ? 1 : 0;
        }
    }
    VGLYPH_CHECK(gray);
    VGLYPH_CHECK(solid > 0);    // the stroke core is fully covered
    VGLYPH_CHECK(partial > 0);  // anti-aliased edges
    VGLYPH_CHECK(pixel(image, 0, 0).r == 0);  // a cell corner stays background
}

VGLYPH_TEST(rasterize_places_cells_row_major) {
    const GlyphAtlas atlas = make_atlas(U" @");
    AsciiFrame frame = make_frame(2, 2, U' ', kWhite, kBlack);
    frame.cells[3].bg = kBlue;  // bottom-right cell
    RgbaImage image;
    VGLYPH_CHECK(rasterize(frame, atlas, image).ok());
    const std::uint32_t w = atlas.cell_px_w();
    const std::uint32_t h = atlas.cell_px_h();
    VGLYPH_CHECK(same(pixel(image, 0, 0), kBlack));
    VGLYPH_CHECK(same(pixel(image, w, h - 1), kBlack));
    VGLYPH_CHECK(same(pixel(image, w - 1, h), kBlack));
    VGLYPH_CHECK(same(pixel(image, w, h), kBlue));
    VGLYPH_CHECK(same(pixel(image, 2 * w - 1, 2 * h - 1), kBlue));
}

VGLYPH_TEST(rasterize_rejects_a_codepoint_outside_the_atlas) {
    const GlyphAtlas atlas = make_atlas(U" .");
    const AsciiFrame frame = make_frame(1, 1, U'A', kWhite, kBlack);
    RgbaImage image;
    const Status s = rasterize(frame, atlas, image);
    VGLYPH_CHECK(!s.ok() && s.error().code == ErrorCode::InvalidArgument);
    VGLYPH_CHECK(s.error().detail.find("U+0041") != std::string::npos);
}

VGLYPH_TEST(rasterize_rejects_a_malformed_frame) {
    const GlyphAtlas atlas = make_atlas(U" .");
    RgbaImage image;
    AsciiFrame empty;
    VGLYPH_CHECK(rasterize(empty, atlas, image).error().code == ErrorCode::InvalidArgument);
    AsciiFrame wrong = make_frame(2, 2, U' ', kWhite, kBlack);
    wrong.cells.pop_back();
    VGLYPH_CHECK(rasterize(wrong, atlas, image).error().code == ErrorCode::InvalidArgument);
}

VGLYPH_TEST(rasterize_enforces_the_size_caps_before_allocating) {
    const GlyphAtlas atlas = make_atlas(U" .");
    RgbaImage image;
    // 2048 columns of 10 px is 20480 px wide, over the 16384 limit.
    const AsciiFrame wide = make_frame(2048, 1, U' ', kWhite, kBlack);
    const Status w = rasterize(wide, atlas, image);
    VGLYPH_CHECK(!w.ok() && w.error().code == ErrorCode::LimitExceeded);
    // 1000 rows of 22 px is 22000 px tall.
    const AsciiFrame tall = make_frame(1, 1000, U' ', kWhite, kBlack);
    const Status t = rasterize(tall, atlas, image);
    VGLYPH_CHECK(!t.ok() && t.error().code == ErrorCode::LimitExceeded);
    // Each side fits (15000 x 15400 px) but the area is over 64 megapixels.
    const AsciiFrame many = make_frame(1500, 700, U' ', kWhite, kBlack);
    const Status m = rasterize(many, atlas, image);
    VGLYPH_CHECK(!m.ok() && m.error().code == ErrorCode::LimitExceeded);
    VGLYPH_CHECK(image.rgba.empty());  // nothing was allocated for the rejected sizes
}

VGLYPH_TEST(rasterize_does_not_allocate_in_a_loop) {
    const GlyphAtlas atlas = make_atlas(U" .:-=+*#%@");
    AsciiFrame frame = make_frame(40, 12, U' ', kWhite, kBlack);
    const std::u32string ramp = U" .:-=+*#%@";
    for (std::size_t i = 0; i < frame.cells.size(); ++i) {
        frame.cells[i].codepoint = i % 7 == 0 ? kUpperHalfBlock : ramp[i % ramp.size()];
        frame.cells[i].fg = Rgb{static_cast<std::uint8_t>(i), 200, 90};
    }
    RgbaImage image;
    VGLYPH_CHECK(rasterize(frame, atlas, image).ok());  // warm-up sizes the buffer
    const std::uint8_t* const data = image.rgba.data();

    g_allocations.store(0);
    g_counting.store(true);
    bool all_ok = true;
    for (int i = 0; i < 50; ++i) {
        all_ok = rasterize(frame, atlas, image).ok() && all_ok;
    }
    g_counting.store(false);
    VGLYPH_CHECK(all_ok);
    // MSVC's debug STL allocates a container proxy for every std::string (each
    // Status holds one), so the count only means something with iterator
    // debugging off; the Release and Linux builds cover it.
#if !defined(_ITERATOR_DEBUG_LEVEL) || _ITERATOR_DEBUG_LEVEL == 0
    VGLYPH_CHECK(g_allocations.load() == 0);
#endif
    VGLYPH_CHECK(image.rgba.data() == data);
}

VGLYPH_TEST_MAIN()
