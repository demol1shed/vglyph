#pragma once

// The terminal player (SPEC section 6.2): grid fitting, single-write frame
// buffering and the pacing rule. The pure pieces are separate functions so
// they can be tested without a terminal.

#include <cstdint>
#include <string>
#include <string_view>

#include "args.hpp"
#include "vglyph/convert.hpp"
#include "vglyph/error.hpp"
#include "vglyph/frame.hpp"

namespace vglyph::cli {

inline constexpr std::uint32_t kMinGridCols = 4;

// Fits the grid of a source of src_w x src_h pixels into a terminal of
// term_cols x term_rows cells and returns `config` with target_cols and
// target_rows set. An explicit width (-w) or height (-H) is only clamped to
// the terminal; otherwise the grid is as wide as the terminal allows while
// its height still fits. A terminal under kMinGridCols x 1 is InvalidArgument.
[[nodiscard]] Result<ConvertConfig> fit_grid(std::uint32_t src_w, std::uint32_t src_h,
                                             std::uint32_t term_cols, std::uint32_t term_rows,
                                             const ConvertConfig& config, bool width_given,
                                             bool height_given);

// True when a frame that is `late_us` past its pts should be dropped instead
// of drawn: it is more than one frame duration late. A frame without a
// duration is never dropped.
[[nodiscard]] bool should_skip(std::int64_t late_us, std::int64_t duration_us) noexcept;

// Serializes frames for the terminal: DEC 2026 synchronized-output markers
// around the .ans text, so the whole frame goes out in one write(2) and the
// terminal shows it atomically. It is the only owner of the output buffer and
// reserves its worst-case size whenever the grid changes, so rendering a frame
// of an unchanged grid never allocates.
class FrameWriter {
public:
    // The returned view stays valid until the next render() call.
    [[nodiscard]] Result<std::string_view> render(const AsciiFrame& frame);

private:
    std::string buf_;
    std::uint32_t cols_ = 0;
    std::uint32_t rows_ = 0;
};

// Plays options.input in the terminal (`vglyph play`): until the media ends,
// or until the user quits with q / Esc (a photo waits for that). Space pauses,
// l toggles looping. The terminal is restored before this returns. Returns
// Cancelled after SIGINT / SIGTERM / Ctrl+C, InvalidArgument when stdin or
// stdout is not a terminal.
[[nodiscard]] Status run_play(const CliOptions& options);

}  // namespace vglyph::cli
