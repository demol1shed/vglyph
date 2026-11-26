#pragma once

// Command-line parsing for the vglyph CLI (SPEC section 8). No OS headers.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include "vglyph/convert.hpp"
#include "vglyph/error.hpp"
#include "vglyph/media_exporter.hpp"

namespace vglyph::cli {

enum class Command { Convert, Play, Info, Help, Version };

// Output formats of `convert`: text, or rasterized through a font.
enum class OutputFormat { Txt, Ans, Png, Gif, Mp4 };

[[nodiscard]] constexpr bool is_visual(OutputFormat format) noexcept {
    return format != OutputFormat::Txt && format != OutputFormat::Ans;
}

struct CliOptions {
    Command command = Command::Help;
    std::filesystem::path input;
    std::filesystem::path output;  // convert only; empty when writing to stdout
    bool to_stdout = false;        // convert only: "-o -"
    OutputFormat format = OutputFormat::Ans;
    ConvertConfig config;
    bool aspect_given = false;  // visual export uses the font's cell shape unless --aspect was given
    std::optional<std::filesystem::path> font;  // png/gif/mp4; none = the embedded font
    std::uint32_t font_px = 16;                 // png/gif/mp4
    int crf = ExportOptions{}.crf;              // mp4 only
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
