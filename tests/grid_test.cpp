#include <cstdint>
#include <limits>

#include "test_framework.hpp"
#include "vglyph/convert.hpp"

using vglyph::ConvertConfig;
using vglyph::ErrorCode;
using vglyph::GridSize;
using vglyph::RenderMode;
using vglyph::Result;
using vglyph::Status;

namespace {

bool has_code(const Status& s, ErrorCode code) { return !s.ok() && s.error().code == code; }

bool has_code(const Result<GridSize>& r, ErrorCode code) {
    return r.error_if() != nullptr && r.error_if()->code == code;
}

// Checks the grid for src_w x src_h and returns it (zeroed on failure).
GridSize grid_of(std::uint32_t src_w, std::uint32_t src_h, const ConvertConfig& config) {
    const Result<GridSize> r = vglyph::compute_grid(src_w, src_h, config);
    const GridSize* grid = r.value_if();
    VGLYPH_CHECK(grid != nullptr);
    return grid != nullptr ? *grid : GridSize{};
}

}  // namespace

VGLYPH_TEST(default_config_is_valid) {
    VGLYPH_CHECK(vglyph::validate(ConvertConfig{}).ok());
}

VGLYPH_TEST(validate_accepts_range_boundaries) {
    ConvertConfig c;
    c.target_cols = 4;
    c.cell_aspect = 0.1f;
    c.brightness = -1.0f;
    c.contrast = 0.0f;
    c.gamma = 0.1f;
    c.charset = U"ab";
    VGLYPH_CHECK(vglyph::validate(c).ok());

    c.target_cols = 2048;
    c.target_rows = 1024;
    c.cell_aspect = 4.0f;
    c.brightness = 1.0f;
    c.contrast = 4.0f;
    c.gamma = 4.0f;
    VGLYPH_CHECK(vglyph::validate(c).ok());
}

VGLYPH_TEST(validate_rejects_out_of_range_values) {
    const ErrorCode bad = ErrorCode::InvalidArgument;
    ConvertConfig c;

    c = ConvertConfig{};
    c.target_cols = 3;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));
    c.target_cols = 0;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));
    c.target_cols = 2049;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));

    c = ConvertConfig{};
    c.target_rows = 1025;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));

    c = ConvertConfig{};
    c.cell_aspect = 0.09f;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));
    c.cell_aspect = 4.1f;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));

    c = ConvertConfig{};
    c.brightness = 1.1f;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));
    c.brightness = -1.1f;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));

    c = ConvertConfig{};
    c.contrast = -0.1f;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));
    c.contrast = 4.1f;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));

    c = ConvertConfig{};
    c.gamma = 0.0f;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));
    c.gamma = 4.1f;
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));

    c = ConvertConfig{};
    c.charset = U"x";
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));
    c.charset.clear();
    VGLYPH_CHECK(has_code(vglyph::validate(c), bad));
}

VGLYPH_TEST(validate_rejects_nan_and_infinity) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    ConvertConfig c;
    c.cell_aspect = nan;
    VGLYPH_CHECK(!vglyph::validate(c).ok());
    c = ConvertConfig{};
    c.brightness = nan;
    VGLYPH_CHECK(!vglyph::validate(c).ok());
    c = ConvertConfig{};
    c.contrast = inf;
    VGLYPH_CHECK(!vglyph::validate(c).ok());
    c = ConvertConfig{};
    c.gamma = nan;
    VGLYPH_CHECK(!vglyph::validate(c).ok());
}

VGLYPH_TEST(grid_square_source) {
    const GridSize g = grid_of(100, 100, ConvertConfig{});  // 100 cols, aspect 0.5
    VGLYPH_CHECK(g.cols == 100);
    VGLYPH_CHECK(g.rows == 50);
    VGLYPH_CHECK(g.sample_w == 100);
    VGLYPH_CHECK(g.sample_h == 50);
    VGLYPH_CHECK(g.sample_bytes == 100U * 50U * 3U);
}

VGLYPH_TEST(grid_wide_and_tall_sources) {
    // 100 * 1080 / 1920 * 0.5 = 28.125 -> 28
    VGLYPH_CHECK(grid_of(1920, 1080, ConvertConfig{}).rows == 28);
    // 100 * 1920 / 1080 * 0.5 = 88.89 -> 89
    VGLYPH_CHECK(grid_of(1080, 1920, ConvertConfig{}).rows == 89);
}

VGLYPH_TEST(grid_cell_aspect_scales_rows) {
    ConvertConfig c;
    c.cell_aspect = 1.0f;
    VGLYPH_CHECK(grid_of(100, 100, c).rows == 100);
    c.cell_aspect = 0.25f;
    VGLYPH_CHECK(grid_of(100, 100, c).rows == 25);
}

VGLYPH_TEST(grid_rows_never_below_one) {
    ConvertConfig c;
    c.target_cols = 4;
    // 4 * 10 / 4000 * 0.5 = 0.005 -> clamped to 1
    VGLYPH_CHECK(grid_of(4000, 10, c).rows == 1);
}

VGLYPH_TEST(grid_explicit_rows_override_source_aspect) {
    ConvertConfig c;
    c.target_rows = 30;
    VGLYPH_CHECK(grid_of(100, 100, c).rows == 30);
    VGLYPH_CHECK(grid_of(1080, 1920, c).rows == 30);
}

VGLYPH_TEST(grid_half_block_doubles_sample_height) {
    ConvertConfig c;
    c.render_mode = RenderMode::HalfBlock;
    const GridSize g = grid_of(100, 100, c);
    VGLYPH_CHECK(g.rows == 50);
    VGLYPH_CHECK(g.sample_w == 100);
    VGLYPH_CHECK(g.sample_h == 100);
    VGLYPH_CHECK(g.sample_bytes == 100U * 100U * 3U);
}

VGLYPH_TEST(grid_row_cap_is_exact) {
    ConvertConfig c;
    c.target_cols = 2048;
    // 2048 * 1000 / 1000 * 0.5 = 1024: allowed.
    VGLYPH_CHECK(grid_of(1000, 1000, c).rows == 1024);
    // 2048 * 1001 / 1000 * 0.5 = 1025.02 -> 1025: LimitExceeded, never cropped.
    VGLYPH_CHECK(has_code(vglyph::compute_grid(1000, 1001, c), ErrorCode::LimitExceeded));
    // An extreme aspect ratio must not overflow either.
    c.target_cols = 4;
    VGLYPH_CHECK(has_code(vglyph::compute_grid(1, 16384, c), ErrorCode::LimitExceeded));
}

VGLYPH_TEST(grid_explicit_rows_cap_is_invalid_argument) {
    ConvertConfig c;
    c.target_rows = 1024;
    VGLYPH_CHECK(grid_of(100, 100, c).rows == 1024);
    c.target_rows = 1025;
    VGLYPH_CHECK(has_code(vglyph::compute_grid(100, 100, c), ErrorCode::InvalidArgument));
}

VGLYPH_TEST(grid_half_block_max_sample_buffer) {
    ConvertConfig c;
    c.render_mode = RenderMode::HalfBlock;
    c.target_cols = 2048;
    c.target_rows = 1024;
    const GridSize g = grid_of(100, 100, c);
    VGLYPH_CHECK(g.sample_w == 2048);
    VGLYPH_CHECK(g.sample_h == 2048);
    VGLYPH_CHECK(g.sample_bytes == 2048ULL * 2048ULL * 3ULL);
}

VGLYPH_TEST(grid_source_dimension_caps) {
    const ConvertConfig c;
    VGLYPH_CHECK(has_code(vglyph::compute_grid(0, 100, c), ErrorCode::LimitExceeded));
    VGLYPH_CHECK(has_code(vglyph::compute_grid(100, 0, c), ErrorCode::LimitExceeded));
    VGLYPH_CHECK(has_code(vglyph::compute_grid(16385, 100, c), ErrorCode::LimitExceeded));
    VGLYPH_CHECK(has_code(vglyph::compute_grid(100, 16385, c), ErrorCode::LimitExceeded));
    VGLYPH_CHECK(vglyph::compute_grid(16384, 1, c).ok());
    VGLYPH_CHECK(has_code(
        vglyph::compute_grid(std::numeric_limits<std::uint32_t>::max(), 1, c),
        ErrorCode::LimitExceeded));
}

VGLYPH_TEST(grid_source_pixel_cap_is_exact) {
    const ConvertConfig c;
    // 16384 * 4096 = 67,108,864 pixels: allowed.
    VGLYPH_CHECK(vglyph::compute_grid(16384, 4096, c).ok());
    VGLYPH_CHECK(has_code(vglyph::compute_grid(16384, 4097, c), ErrorCode::LimitExceeded));
}

VGLYPH_TEST(grid_rejects_invalid_config) {
    ConvertConfig c;
    c.target_cols = 3;
    VGLYPH_CHECK(has_code(vglyph::compute_grid(100, 100, c), ErrorCode::InvalidArgument));
}

VGLYPH_TEST_MAIN()
