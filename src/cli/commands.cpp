#include "commands.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

#include "platform.hpp"
#include "vglyph/convert.hpp"
#include "vglyph/frame.hpp"
#include "vglyph/text_serializer.hpp"
#include "vglyph/version.hpp"

namespace vglyph::cli {

namespace {

[[nodiscard]] const char* kind_name(MediaKind kind) noexcept {
    switch (kind) {
        case MediaKind::Image:
            return "image";
        case MediaKind::AnimatedImage:
            return "animated-image";
        case MediaKind::Video:
            return "video";
    }
    return "unknown";
}

[[nodiscard]] std::string utf8_string(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

void report(const Error& error) {
    // A failed write to stderr leaves nowhere to report it; the exit code still tells.
    static_cast<void>(write_stderr("vglyph: " + error.detail + "\n"));
}

// Writes the finished output to a file or stdout. The file is opened only
// after the content exists, so a failed conversion never truncates it.
[[nodiscard]] Status write_output(const CliOptions& options, std::string_view data) {
    if (options.to_stdout) {
        return write_stdout(data);
    }
    std::ofstream file(options.output, std::ios::binary);
    if (!file) {
        return make_error(ErrorCode::IoError, "cannot open " + utf8_string(options.output) + " for writing");
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    if (file.fail()) {
        return make_error(ErrorCode::IoError, "cannot write " + utf8_string(options.output));
    }
    return Status::success();
}

[[nodiscard]] Status run_info(const CliOptions& options) {
    Result<MediaReader> opened = MediaReader::open(options.input);
    const MediaReader* reader = opened.value_if();
    if (reader == nullptr) {
        return *opened.error_if();
    }
    return write_stdout(format_info(reader->info()));
}

[[nodiscard]] Status run_convert(const CliOptions& options) {
    std::string content;
    if (const Status s = convert_frame(options, content); !s.ok()) {
        return s;
    }
    return write_output(options, content);
}

}  // namespace

int exit_code(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:
            return 0;
        case ErrorCode::InvalidArgument:
            return 2;
        case ErrorCode::Cancelled:
            return 130;
        case ErrorCode::IoError:
        case ErrorCode::UnsupportedFormat:
        case ErrorCode::DecodeError:
        case ErrorCode::EncodeError:
        case ErrorCode::FontError:
        case ErrorCode::LimitExceeded:
        case ErrorCode::InternalError:
            break;
    }
    return 1;
}

std::string format_info(const MediaInfo& info) {
    // snprintf, not iostreams: the program never calls setlocale, so "%.3f" is locale-independent.
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "%.3f", info.avg_fps);
    const std::string fps = n > 0 && static_cast<std::size_t>(n) < sizeof buf ? buf : "unknown";
    std::string out;
    out += std::string("kind: ") + kind_name(info.media_kind) + "\n";
    out += "width: " + std::to_string(info.src_width) + "\n";
    out += "height: " + std::to_string(info.src_height) + "\n";
    out += "pixel_format: " + info.pixel_format_name + "\n";
    out += "codec: " + info.codec_name + "\n";
    out += "fps: " + fps + "\n";
    out += "duration_us: " + std::to_string(info.duration_us) + "\n";
    out += std::string("has_audio: ") + (info.has_audio ? "true" : "false") + "\n";
    return out;
}

// shortcut: decodes and scales every frame before frame_index; fine for a
// one-shot convert, upgrade if a frame-accurate seek is ever needed.
Status convert_frame(const CliOptions& options, std::string& out) {
    Result<MediaReader> opened = MediaReader::open(options.input);
    MediaReader* reader = opened.value_if();
    if (reader == nullptr) {
        return *opened.error_if();
    }
    const MediaInfo& info = reader->info();
    const Result<GridSize> grid =
        compute_grid(info.src_width, info.src_height, options.config);
    const GridSize* size = grid.value_if();
    if (size == nullptr) {
        return *grid.error_if();
    }

    VideoFrame video;
    for (std::uint64_t index = 0;; ++index) {
        const Result<ReadStatus> read = reader->read_frame(size->sample_w, size->sample_h, video);
        const ReadStatus* status = read.value_if();
        if (status == nullptr) {
            return *read.error_if();
        }
        if (*status == ReadStatus::EndOfStream) {
            return make_error(ErrorCode::InvalidArgument,
                              "--frame " + std::to_string(options.frame_index) +
                                  " is past the last frame (the input has " +
                                  std::to_string(index) + ")");
        }
        if (index == options.frame_index) {
            break;
        }
    }

    AsciiFrame frame;
    if (const Status s = convert(video.rgb24, video.width, video.height, options.config, frame);
        !s.ok()) {
        return s;
    }

    const bool ansi = options.format == TextFormat::Ans;
    const Result<std::size_t> capacity = ansi ? ansi_capacity(frame.cols, frame.rows)
                                              : text_capacity(frame.cols, frame.rows);
    if (capacity.value_if() == nullptr) {
        return *capacity.error_if();
    }
    // The capacity counts a line break after the last row, which is the final newline.
    out.reserve(out.size() + *capacity.value_if());
    if (const Status s = ansi ? append_ansi(frame, out) : append_text(frame, out); !s.ok()) {
        return s;
    }
    out.push_back('\n');
    return Status::success();
}

int run(std::span<const std::string_view> args) {
    const Result<CliOptions> parsed = parse_args(args);
    const CliOptions* options = parsed.value_if();
    if (options == nullptr) {
        report(*parsed.error_if());
        static_cast<void>(write_stderr("Try 'vglyph --help'.\n"));
        return exit_code(parsed.error_if()->code);
    }

    Status result = Status::success();
    switch (options->command) {
        case Command::Help:
            result = write_stdout(usage_text());
            break;
        case Command::Version:
            result = write_stdout(std::string("vglyph ") + version_string() + "\nLicense: GPLv3\n");
            break;
        case Command::Info:
            result = run_info(*options);
            break;
        case Command::Convert:
            result = run_convert(*options);
            break;
        case Command::Play:
            result = make_error(ErrorCode::InvalidArgument, "the play command is not implemented yet");
            break;
    }
    if (!result.ok()) {
        report(result.error());
        return exit_code(result.error().code);
    }
    return 0;
}

}  // namespace vglyph::cli
