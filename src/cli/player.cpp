#include "player.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

#include "platform.hpp"
#include "vglyph/media_reader.hpp"
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

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::microseconds;

constexpr std::string_view kClearScreen = "\x1b[2J";
constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max() / 2;

enum class Wait { Due, Resized, Stop };

// One playback session. Media time is measured against `origin_`: a frame with
// pts P is due when steady_clock reaches origin_ + P.
class Player {
public:
    Player(MediaReader&& reader, Terminal&& terminal, const CliOptions& options)
        : reader_(std::move(reader)), term_(std::move(terminal)), options_(options),
          loop_(options.loop) {}

    [[nodiscard]] Status run();

private:
    [[nodiscard]] std::int64_t now_us() const {
        return std::chrono::duration_cast<microseconds>(Clock::now() - origin_).count();
    }
    [[nodiscard]] Status fit();
    [[nodiscard]] Status relayout();
    [[nodiscard]] Status draw();
    [[nodiscard]] Wait wait_until(std::int64_t due_us);
    void toggle_pause();

    MediaReader reader_;
    Terminal term_;
    const CliOptions& options_;

    ConvertConfig config_;
    GridSize grid_;
    // Reused for every frame: no allocation in the loop once the grid is set.
    VideoFrame video_;
    AsciiFrame ascii_;
    FrameWriter writer_;

    Clock::time_point origin_ = Clock::now();
    Clock::time_point paused_at_;
    bool paused_ = false;
    bool loop_;
    bool resync_ = true;        // the next frame read starts the clock (start, loop restart)
    bool redraw_ = false;       // the next frame read is the shown one again: draw it at once
    bool skipped_last_ = false;
    bool interrupted_ = false;
    std::int64_t shown_pts_us_ = 0;  // the frame on screen
    std::int64_t shown_end_us_ = 0;  // when it stops being current
};

Status Player::fit() {
    const Result<TerminalSize> size = term_.size();
    if (size.value_if() == nullptr) {
        return *size.error_if();
    }
    const MediaInfo& info = reader_.info();
    const Result<ConvertConfig> fitted =
        fit_grid(info.src_width, info.src_height, size.value_if()->cols, size.value_if()->rows,
                 options_.config, options_.width_given, options_.height_given);
    if (fitted.value_if() == nullptr) {
        return *fitted.error_if();
    }
    config_ = *fitted.value_if();
    const Result<GridSize> grid = compute_grid(info.src_width, info.src_height, config_);
    if (grid.value_if() == nullptr) {
        return *grid.error_if();
    }
    grid_ = *grid.value_if();
    return Status::success();
}

// The terminal changed size: clear it, refit the grid and arrange to draw the
// frame that was on screen again, at the new size.
Status Player::relayout() {
    if (const Status s = write_stdout(kClearScreen); !s.ok()) {
        return s;
    }
    if (const Status s = fit(); !s.ok()) {
        return s;
    }
    if (const Status s = reader_.seek_us(shown_pts_us_); !s.ok()) {
        return s;
    }
    redraw_ = true;
    return Status::success();
}

Status Player::draw() {
    if (const Status s = convert(video_.rgb24, video_.width, video_.height, config_, ascii_); !s.ok()) {
        return s;
    }
    const Result<std::string_view> bytes = writer_.render(ascii_);
    if (bytes.value_if() == nullptr) {
        return *bytes.error_if();
    }
    shown_pts_us_ = video_.pts_us;
    shown_end_us_ = video_.pts_us + video_.duration_us;
    return write_stdout(*bytes.value_if());
}

void Player::toggle_pause() {
    if (paused_) {
        origin_ += Clock::now() - paused_at_;  // the pause does not count as playing time
        paused_ = false;
    } else {
        paused_at_ = Clock::now();
        paused_ = true;
    }
}

// Waits until media time reaches `due_us` (never, while paused), serving keys
// on the way. Polls the terminal at least once, so input is read even when
// every frame is late.
Wait Player::wait_until(std::int64_t due_us) {
    for (;;) {
        microseconds timeout{kNever};
        bool due = false;
        if (!paused_) {
            const std::int64_t now = now_us();
            due = now >= due_us;
            timeout = due ? microseconds::zero() : microseconds(due_us - now);
        }
        switch (term_.wait(timeout)) {
            case TerminalEvent::None:
                if (due) {
                    return Wait::Due;
                }
                break;
            case TerminalEvent::Resized:
                return Wait::Resized;
            case TerminalEvent::Interrupted:
                interrupted_ = true;
                return Wait::Stop;
            case TerminalEvent::Quit:
                return Wait::Stop;
            case TerminalEvent::TogglePause:
                toggle_pause();
                break;
            case TerminalEvent::ToggleLoop:
                loop_ = !loop_;
                break;
        }
    }
}

Status Player::run() {
    if (const Status s = fit(); !s.ok()) {
        return s;
    }
    const bool still = reader_.info().media_kind == MediaKind::Image;

    for (;;) {
        const Result<ReadStatus> read = reader_.read_frame(grid_.sample_w, grid_.sample_h, video_);
        const ReadStatus* status = read.value_if();
        if (status == nullptr) {
            return *read.error_if();
        }

        if (*status == ReadStatus::EndOfStream) {
            if (resync_) {
                return Status::success();  // nothing was playable (a loop restart found no frame)
            }
            // The last frame stays up for its own duration; a photo stays until the user quits.
            const Wait waited = wait_until(still ? kNever : shown_end_us_);
            if (waited == Wait::Stop) {
                break;
            }
            if (waited == Wait::Resized) {
                if (const Status s = relayout(); !s.ok()) {
                    return s;
                }
                continue;
            }
            if (!loop_) {
                return Status::success();
            }
            if (const Status s = reader_.seek_us(0); !s.ok()) {
                return s;
            }
            resync_ = true;
            continue;
        }

        if (resync_) {
            origin_ = Clock::now() - microseconds(video_.pts_us);
            resync_ = false;
        }
        if (redraw_) {
            redraw_ = false;
            skipped_last_ = false;
            if (const Status s = draw(); !s.ok()) {
                return s;
            }
            continue;
        }

        const Wait waited = wait_until(video_.pts_us);
        if (waited == Wait::Stop) {
            break;
        }
        if (waited == Wait::Resized) {
            if (const Status s = relayout(); !s.ok()) {
                return s;
            }
            continue;
        }
        // A frame more than its own duration late is dropped, but never two in a
        // row, so playback that cannot keep up still moves instead of freezing.
        if (!skipped_last_ && should_skip(now_us() - video_.pts_us, video_.duration_us)) {
            skipped_last_ = true;
            continue;
        }
        skipped_last_ = false;
        if (const Status s = draw(); !s.ok()) {
            return s;
        }
    }
    return interrupted_ ? Status(make_error(ErrorCode::Cancelled, "interrupted")) : Status::success();
}

}  // namespace

Status run_play(const CliOptions& options) {
    Result<MediaReader> opened = MediaReader::open(options.input);
    MediaReader* reader = opened.value_if();
    if (reader == nullptr) {
        return *opened.error_if();
    }
    Result<Terminal> terminal = Terminal::open();
    Terminal* guard = terminal.value_if();
    if (guard == nullptr) {
        return *terminal.error_if();
    }
    // `player` is destroyed first, which restores the terminal before the caller reports anything.
    Player player(std::move(*reader), std::move(*guard), options);
    return player.run();
}

}  // namespace vglyph::cli
