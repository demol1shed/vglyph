#pragma once

// Command-line parsing for the vglyph CLI (SPEC section 8). No OS headers.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "vglyph/convert.hpp"
#include "vglyph/error.hpp"

namespace vglyph::cli {

enum class Command { Convert, Play, Info, Help, Version };

// Output formats `convert` can write today. PNG, GIF and MP4 arrive with the
// exporters and are rejected as "not implemented" until then.
enum class TextFormat { Txt, Ans };

struct CliOptions {
    Command command = Command::Help;
    std::filesystem::path input;
    std::filesystem::path output;  // convert only; empty when writing to stdout
    bool to_stdout = false;        // convert only: "-o -"
    TextFormat format = TextFormat::Ans;
    ConvertConfig config;
    bool width_given = false;   // play fits the grid to the terminal unless -w was given
    bool height_given = false;  // ... or -H
    std::uint64_t frame_index = 0;  // convert only: --frame
    bool loop = false;              // play only: --loop
};

// Parses the arguments after argv[0]. Every failure is InvalidArgument, which
// the CLI reports with exit code 2. A config that is out of range is rejected
// here, so later stages can rely on validate(options.config) being ok.
[[nodiscard]] Result<CliOptions> parse_args(std::span<const std::string_view> args);

[[nodiscard]] std::string_view usage_text() noexcept;

// Builds a path from UTF-8 bytes (argv is UTF-8 on every platform: Linux
// terminals by convention, Windows through the executable's manifest).
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8);

}  // namespace vglyph::cli
