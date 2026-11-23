#include "player.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

#include "vglyph/text_serializer.hpp"

namespace vglyph::cli {

namespace {

constexpr std::string_view kSyncBegin = "\x1b[?2026h\x1b[H";  // begin synchronized update, home
constexpr std::string_view kSyncEnd = "\x1b[?2026l";

}  // namespace

Result<ConvertConfig> fit_grid(std::uint32_t src_w, std::uint32_t src_h, std::uint32_t term_cols,
                               std::uint32_t term_rows, const ConvertConfig& config,
                               bool width_given, bool height_given) {
    if (src_w == 0 || src_h == 0) {
        return make_error(ErrorCode::InvalidArgument, "source dimensions must be non-zero");
    }
    if (term_cols < kMinGridCols || term_rows < 1) {
        return make_error(ErrorCode::InvalidArgument,
                          "the terminal is too small (" + std::to_string(term_cols) + "x" +
                              std::to_string(term_rows) + " cells, need at least " +
                              std::to_string(kMinGridCols) + "x1)");
    }
    const std::uint32_t max_cols = std::min(term_cols, kMaxGridCols);
    const std::uint32_t max_rows = std::min(term_rows, kMaxGridRows);

    ConvertConfig fitted = config;
    fitted.target_cols = width_given ? std::min(config.target_cols, max_cols) : max_cols;

    if (height_given && config.target_rows != 0) {
        fitted.target_rows = std::min(config.target_rows, max_rows);
        if (!width_given) {
            // Only the height was asked for: keep the picture's aspect ratio.
            const double cols = std::round(static_cast<double>(fitted.target_rows) *
                                           static_cast<double>(src_w) /
                                           (static_cast<double>(src_h) * config.cell_aspect));
            fitted.target_cols = static_cast<std::uint32_t>(
                std::clamp(cols, static_cast<double>(kMinGridCols), static_cast<double>(max_cols)));
        }
        return fitted;
    }

    // Automatic height: shrink the columns until the derived rows fit. A grid
    // taller than kMaxGridRows (a LimitExceeded error) also means "shrink".
    fitted.target_rows = 0;
    for (;;) {
        const Result<GridSize> grid = compute_grid(src_w, src_h, fitted);
        const GridSize* size = grid.value_if();
        if (size != nullptr && size->rows <= max_rows) {
            return fitted;
        }
        if (fitted.target_cols <= kMinGridCols) {
            if (size == nullptr) {
                return *grid.error_if();
            }
            // An extremely tall source: stretch it to the terminal height.
            fitted.target_rows = max_rows;
            return fitted;
        }
        --fitted.target_cols;
    }
}

bool should_skip(std::int64_t late_us, std::int64_t duration_us) noexcept {
    return duration_us > 0 && late_us > duration_us;
}

Result<std::string_view> FrameWriter::render(const AsciiFrame& frame) {
    if (frame.cols != cols_ || frame.rows != rows_) {
        const Result<std::size_t> capacity = ansi_capacity(frame.cols, frame.rows);
        const std::size_t* bytes = capacity.value_if();
        if (bytes == nullptr) {
            return *capacity.error_if();
        }
        const std::size_t wrapper = kSyncBegin.size() + kSyncEnd.size();
        if (*bytes > std::numeric_limits<std::size_t>::max() - wrapper) {
            return make_error(ErrorCode::LimitExceeded, "frame buffer size overflows");
        }
        buf_.reserve(*bytes + wrapper);
        cols_ = frame.cols;
        rows_ = frame.rows;
    }
    buf_.clear();
    buf_.append(kSyncBegin);
    if (const Status s = append_ansi(frame, buf_); !s.ok()) {
        return s.error();
    }
    buf_.append(kSyncEnd);
    return std::string_view(buf_);
}

}  // namespace vglyph::cli
