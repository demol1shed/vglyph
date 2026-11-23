#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "player.hpp"
#include "test_framework.hpp"
#include "vglyph/convert.hpp"
#include "vglyph/text_serializer.hpp"

using namespace vglyph;
using namespace vglyph::cli;

// ---- Allocation counter ----------------------------------------------------
// Replaces the global allocation functions for this test binary. Only
// allocations made while g_counting is set are counted, so the framework's
// own bookkeeping stays out of the result. Every new form is paired with a
// matching delete so ASan sees malloc/free consistently.

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

constexpr std::string_view kSyncBegin = "\x1b[?2026h\x1b[H";
constexpr std::string_view kSyncEnd = "\x1b[?2026l";

// A half-block, two-color config: the most expensive output per cell.
ConvertConfig worst_case_config(std::uint32_t cols) {
    ConvertConfig c;
    c.target_cols = cols;
    c.render_mode = RenderMode::HalfBlock;
    c.color_mode = ColorMode::TrueColorFgBg;
    return c;
}

// Pixels whose neighbors differ in every channel, so no color run repeats.
std::vector<std::uint8_t> noisy_rgb(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t i = 0; i < px.size(); ++i) {
        px[i] = static_cast<std::uint8_t>((i * 197 + (i / 7) * 59 + 13) % 251 + 4);
    }
    return px;
}

std::vector<std::uint8_t> black_rgb(std::uint32_t w, std::uint32_t h) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 3, 0);
}

GridSize grid_of(std::uint32_t src_w, std::uint32_t src_h, const ConvertConfig& config) {
    const Result<GridSize> grid = compute_grid(src_w, src_h, config);
    const GridSize* size = grid.value_if();
    VGLYPH_CHECK(size != nullptr);
    return size != nullptr ? *size : GridSize{};
}

ConvertConfig fit(std::uint32_t src_w, std::uint32_t src_h, std::uint32_t term_cols,
                  std::uint32_t term_rows, const ConvertConfig& config, bool width_given,
                  bool height_given) {
    const Result<ConvertConfig> fitted =
        fit_grid(src_w, src_h, term_cols, term_rows, config, width_given, height_given);
    const ConvertConfig* c = fitted.value_if();
    VGLYPH_CHECK(c != nullptr);
    return c != nullptr ? *c : ConvertConfig{};
}

}  // namespace

// ---- FrameWriter -----------------------------------------------------------

VGLYPH_TEST(frame_writer_wraps_the_ansi_text_in_synchronized_output) {
    const ConvertConfig config = worst_case_config(8);
    const GridSize grid = grid_of(16, 16, config);
    AsciiFrame frame;
    VGLYPH_CHECK(convert(noisy_rgb(grid.sample_w, grid.sample_h), grid.sample_w, grid.sample_h,
                         config, frame)
                     .ok());

    std::string expected(kSyncBegin);
    VGLYPH_CHECK(append_ansi(frame, expected).ok());
    expected.append(kSyncEnd);

    FrameWriter writer;
    const Result<std::string_view> out = writer.render(frame);
    VGLYPH_CHECK(out.value_if() != nullptr && *out.value_if() == expected);
    // The text is one frame: DEC 2026 begin + home first, end last, no trailing newline.
    VGLYPH_CHECK(expected.size() > kSyncBegin.size() + kSyncEnd.size());
    VGLYPH_CHECK(expected.back() == 'l');
}

VGLYPH_TEST(frame_writer_rejects_a_malformed_frame) {
    AsciiFrame frame;
    frame.cols = 4;
    frame.rows = 2;  // no cells
    FrameWriter writer;
    const Result<std::string_view> out = writer.render(frame);
    VGLYPH_CHECK(out.error_if() != nullptr &&
                 out.error_if()->code == ErrorCode::InvalidArgument);
}

VGLYPH_TEST(frame_writer_does_not_allocate_after_the_grid_is_set) {
    constexpr std::uint32_t kCols = 80;
    const ConvertConfig config = worst_case_config(kCols);
    const GridSize grid = grid_of(160, 90, config);
    const std::vector<std::uint8_t> quiet = black_rgb(grid.sample_w, grid.sample_h);
    const std::vector<std::uint8_t> busy = noisy_rgb(grid.sample_w, grid.sample_h);

    // Warm up with the cheapest possible frame: if the writer sized its buffer
    // from the first frame's bytes instead of the grid, the busy frames below
    // would have to grow it.
    AsciiFrame frame;
    FrameWriter writer;
    VGLYPH_CHECK(convert(quiet, grid.sample_w, grid.sample_h, config, frame).ok());
    VGLYPH_CHECK(writer.render(frame).value_if() != nullptr);

    constexpr int kFrames = 50;
    std::size_t longest = 0;
    g_allocations.store(0);
    g_counting.store(true);
    for (int i = 0; i < kFrames; ++i) {
        const Status converted = convert(busy, grid.sample_w, grid.sample_h, config, frame);
        const Result<std::string_view> out = writer.render(frame);
        const std::string_view* view = out.value_if();
        if (!converted.ok() || view == nullptr) {
            g_counting.store(false);
            VGLYPH_CHECK(converted.ok() && view != nullptr);
            return;
        }
        longest = std::max(longest, view->size());
    }
    g_counting.store(false);

    // MSVC's debug STL allocates a container proxy whenever a std::string is
    // constructed (every Status and Error), so the count only means something
    // with iterator debugging off; the Release builds cover it.
#if !defined(_ITERATOR_DEBUG_LEVEL) || _ITERATOR_DEBUG_LEVEL == 0
    VGLYPH_CHECK(g_allocations.load() == 0);
#endif
    // The busy frames really were large, so the buffer had to be sized up front.
    VGLYPH_CHECK(longest > 20 * static_cast<std::size_t>(kCols) * grid.rows);
}

// ---- fit_grid --------------------------------------------------------------

VGLYPH_TEST(fit_grid_fills_the_terminal_width_of_a_wide_source) {
    const ConvertConfig fitted = fit(1920, 1080, 120, 40, ConvertConfig{}, false, false);
    VGLYPH_CHECK(fitted.target_cols == 120);
    const GridSize grid = grid_of(1920, 1080, fitted);
    VGLYPH_CHECK(grid.rows == 34);  // round(120 * 1080/1920 * 0.5)
}

VGLYPH_TEST(fit_grid_shrinks_the_width_of_a_tall_source_to_fit_the_height) {
    const ConvertConfig fitted = fit(100, 200, 80, 24, ConvertConfig{}, false, false);
    const GridSize grid = grid_of(100, 200, fitted);
    VGLYPH_CHECK(grid.rows <= 24);
    VGLYPH_CHECK(grid.rows == 24 && fitted.target_cols == 24);  // rows == cols for 1:2 at 0.5
}

VGLYPH_TEST(fit_grid_clamps_an_explicit_width_to_the_terminal) {
    ConvertConfig wide;
    wide.target_cols = 500;
    VGLYPH_CHECK(fit(1920, 1080, 80, 40, wide, true, false).target_cols == 80);

    ConvertConfig narrow;
    narrow.target_cols = 40;
    VGLYPH_CHECK(fit(1920, 1080, 80, 40, narrow, true, false).target_cols == 40);
}

VGLYPH_TEST(fit_grid_still_fits_the_height_when_only_a_width_was_given) {
    ConvertConfig config;
    config.target_cols = 80;
    const ConvertConfig fitted = fit(100, 200, 80, 24, config, true, false);
    VGLYPH_CHECK(grid_of(100, 200, fitted).rows <= 24);
}

VGLYPH_TEST(fit_grid_derives_the_width_from_an_explicit_height) {
    ConvertConfig config;
    config.target_rows = 10;
    const ConvertConfig fitted = fit(200, 100, 120, 40, config, false, true);
    VGLYPH_CHECK(fitted.target_rows == 10);
    VGLYPH_CHECK(fitted.target_cols == 40);  // round(10 * 200/100 / 0.5)
}

VGLYPH_TEST(fit_grid_clamps_an_explicit_height_to_the_terminal) {
    ConvertConfig config;
    config.target_rows = 100;
    config.target_cols = 60;
    const ConvertConfig fitted = fit(200, 100, 120, 24, config, true, true);
    VGLYPH_CHECK(fitted.target_rows == 24);
    VGLYPH_CHECK(fitted.target_cols == 60);
}

VGLYPH_TEST(fit_grid_treats_height_zero_as_automatic) {
    ConvertConfig config;
    config.target_rows = 0;
    const ConvertConfig fitted = fit(200, 100, 80, 24, config, false, true);
    VGLYPH_CHECK(fitted.target_cols == 80 && fitted.target_rows == 0);
}

VGLYPH_TEST(fit_grid_half_block_counts_cell_rows_not_pixel_rows) {
    ConvertConfig config;
    config.render_mode = RenderMode::HalfBlock;
    const ConvertConfig fitted = fit(100, 200, 80, 24, config, false, false);
    const GridSize grid = grid_of(100, 200, fitted);
    VGLYPH_CHECK(grid.rows == 24);
    VGLYPH_CHECK(grid.sample_h == 48);
}

VGLYPH_TEST(fit_grid_stretches_an_extreme_source_instead_of_failing) {
    // 10 x 1000 at 4 columns would need 200 rows; the terminal has 24.
    const ConvertConfig fitted = fit(10, 1000, 80, 24, ConvertConfig{}, false, false);
    VGLYPH_CHECK(fitted.target_cols == kMinGridCols);
    VGLYPH_CHECK(fitted.target_rows == 24);
}

VGLYPH_TEST(fit_grid_rejects_a_tiny_terminal) {
    for (const auto& size : {std::pair<std::uint32_t, std::uint32_t>{3, 24}, {80, 0}, {0, 0}}) {
        const Result<ConvertConfig> fitted =
            fit_grid(100, 100, size.first, size.second, ConvertConfig{}, false, false);
        VGLYPH_CHECK(fitted.error_if() != nullptr &&
                     fitted.error_if()->code == ErrorCode::InvalidArgument);
    }
    // The smallest usable terminal still works.
    VGLYPH_CHECK(fit_grid(100, 100, 4, 1, ConvertConfig{}, false, false).value_if() != nullptr);
}

VGLYPH_TEST(fit_grid_rejects_an_empty_source) {
    const Result<ConvertConfig> fitted = fit_grid(0, 100, 80, 24, ConvertConfig{}, false, false);
    VGLYPH_CHECK(fitted.error_if() != nullptr &&
                 fitted.error_if()->code == ErrorCode::InvalidArgument);
}

// ---- should_skip -----------------------------------------------------------

VGLYPH_TEST(should_skip_only_when_more_than_one_frame_late) {
    VGLYPH_CHECK(!should_skip(-5000, 40000));   // early
    VGLYPH_CHECK(!should_skip(0, 40000));       // on time
    VGLYPH_CHECK(!should_skip(40000, 40000));   // exactly one frame late
    VGLYPH_CHECK(should_skip(40001, 40000));    // more than one frame late
}

VGLYPH_TEST(should_skip_never_drops_a_frame_without_a_duration) {
    VGLYPH_CHECK(!should_skip(1'000'000, 0));
    VGLYPH_CHECK(!should_skip(1'000'000, -1));
}

VGLYPH_TEST_MAIN()
