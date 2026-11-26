#pragma once

// The `info` and `convert` commands and the exit-code contract (SPEC 8.3).

#include <span>
#include <string>
#include <string_view>

#include "args.hpp"
#include "vglyph/error.hpp"
#include "vglyph/media_reader.hpp"

namespace vglyph::cli {

// Process exit code for a failed operation: InvalidArgument (bad usage,
// including a --frame past the end) is 2, Cancelled is 130, the rest 1.
[[nodiscard]] int exit_code(ErrorCode code) noexcept;

// `info` output: one "key: value" line per MediaInfo field.
[[nodiscard]] std::string format_info(const MediaInfo& info);

// For txt and ans output: converts frame options.frame_index of options.input
// and appends the finished content, including the final newline, to `out`.
// A frame index past the last frame is InvalidArgument.
[[nodiscard]] Status convert_frame(const CliOptions& options, std::string& out);

// Runs the command line (arguments after argv[0]) and returns the exit code.
// Output goes to stdout, diagnostics to stderr.
[[nodiscard]] int run(std::span<const std::string_view> args);

}  // namespace vglyph::cli
