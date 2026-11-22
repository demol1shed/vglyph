#include "vglyph/convert.hpp"

#include <cmath>
#include <string>

namespace vglyph {

namespace {

// Moves to a shared header when Phase 2 needs it.
[[nodiscard]] bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }
    out = a * b;
    return true;
}

// False for NaN, since every comparison with NaN is false.
[[nodiscard]] bool in_range(float v, float lo, float hi) noexcept { return v >= lo && v <= hi; }

[[nodiscard]] Error invalid(const char* field, const char* range) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(field) + " must be in " + range);
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
    return Status::success();
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
    grid.sample_w = grid.cols;
    grid.sample_h = config.render_mode == RenderMode::HalfBlock ? rows * 2 : rows;

    std::uint64_t sample_pixels = 0;
    if (!checked_mul(grid.sample_w, grid.sample_h, sample_pixels) ||
        !checked_mul(sample_pixels, 3, grid.sample_bytes)) {
        return make_error(ErrorCode::LimitExceeded, "sampling buffer size overflows");
    }
    return grid;
}

}  // namespace vglyph
