#include "commands.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "platform.hpp"
#include "player.hpp"
#include "vglyph/convert.hpp"
#include "vglyph/frame.hpp"
#include "vglyph/media_exporter.hpp"
#include "vglyph/rasterizer.hpp"
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

[[nodiscard]] Error interrupted() { return make_error(ErrorCode::Cancelled, "interrupted"); }

// Reads up to frame number `index` into `video`. `interrupt` may be null.
// shortcut: decodes and scales every frame before `index`; fine for a
// one-shot convert, upgrade if a frame-accurate seek is ever needed.
[[nodiscard]] Status read_frame_at(MediaReader& reader, const GridSize& grid, Rgb background,
                                   std::uint64_t index, const InterruptGuard* interrupt,
                                   VideoFrame& video) {
    for (std::uint64_t i = 0;; ++i) {
        if (interrupt != nullptr && interrupt->requested()) {
            return interrupted();
        }
        const Result<ReadStatus> read =
            reader.read_frame(grid.sample_w, grid.sample_h, video, background);
        const ReadStatus* status = read.value_if();
        if (status == nullptr) {
            return *read.error_if();
        }
        if (*status == ReadStatus::EndOfStream) {
            return make_error(ErrorCode::InvalidArgument,
                              "--frame " + std::to_string(index) +
                                  " is past the last frame (the input has " + std::to_string(i) +
                                  ")");
        }
        if (i == index) {
            return Status::success();
        }
    }
}

// ---- Visual export (png, gif, mp4) ------------------------------------------

constexpr std::size_t kMaxFontBytes = 16 * 1024 * 1024;

[[nodiscard]] Result<std::vector<std::uint8_t>> read_font(const std::filesystem::path& path) {
    const std::string cannot_read = "cannot read the font file " + utf8_string(path);
    // Only regular files: reading a directory or a device does not fail cleanly.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return make_error(ErrorCode::IoError, cannot_read);
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return make_error(ErrorCode::IoError, cannot_read);
    }
    // One byte over the cap tells a too large file from one that just fits.
    std::vector<std::uint8_t> bytes(kMaxFontBytes + 1);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (file.bad()) {
        return make_error(ErrorCode::IoError, cannot_read);
    }
    const auto size = static_cast<std::size_t>(file.gcount());
    if (size > kMaxFontBytes) {
        return make_error(ErrorCode::LimitExceeded,
                          "the font file " + utf8_string(path) + " is larger than 16 MiB");
    }
    bytes.resize(size);
    return bytes;
}

[[nodiscard]] Result<GlyphAtlas> load_atlas(const CliOptions& options) {
    if (!options.font.has_value()) {
        return GlyphAtlas::create_default(options.font_px, options.config.charset);
    }
    const Result<std::vector<std::uint8_t>> font = read_font(*options.font);
    const std::vector<std::uint8_t>* bytes = font.value_if();
    if (bytes == nullptr) {
        return *font.error_if();
    }
    return GlyphAtlas::create(*bytes, options.font_px, options.config.charset);
}

[[nodiscard]] ExportFormat export_format(OutputFormat format) noexcept {
    switch (format) {
        case OutputFormat::Gif:
            return ExportFormat::Gif;
        case OutputFormat::Mp4:
            return ExportFormat::Mp4;
        case OutputFormat::Png:
        case OutputFormat::Txt:  // text formats never get here
        case OutputFormat::Ans:
            break;
    }
    return ExportFormat::Png;
}

// Converts, rasterizes and encodes one video frame after another, reusing its
// buffers. The first frame creates the output file and fixes its pixel size.
class VisualWriter {
public:
    VisualWriter(const std::filesystem::path& path, const CliOptions& options,
                 const ConvertConfig& config, const GlyphAtlas& atlas)
        : path_(path), options_(options), config_(config), atlas_(atlas) {}

    [[nodiscard]] Status add(const VideoFrame& video, std::int64_t pts_us) {
        if (Status s = convert(video.rgb24, video.width, video.height, config_, frame_); !s.ok()) {
            return s;
        }
        if (Status s = rasterize(frame_, atlas_, image_); !s.ok()) {
            return s;
        }
        if (!exporter_.has_value()) {
            Result<MediaExporter> opened =
                MediaExporter::open(path_, export_format(options_.format), image_.width,
                                    image_.height, ExportOptions{options_.crf, config_.bg_color});
            MediaExporter* exporter = opened.value_if();
            if (exporter == nullptr) {
                return *opened.error_if();
            }
            exporter_.emplace(std::move(*exporter));
        }
        return exporter_->write_frame(image_, pts_us, video.duration_us);
    }

    [[nodiscard]] Status finish() {
        if (!exporter_.has_value()) {
            return make_error(ErrorCode::DecodeError, "the input has no video frames");
        }
        return exporter_->finish();
    }

private:
    const std::filesystem::path& path_;
    const CliOptions& options_;
    const ConvertConfig& config_;
    const GlyphAtlas& atlas_;
    AsciiFrame frame_;
    RgbaImage image_;
    std::optional<MediaExporter> exporter_;
};

// Streams every frame to `writer`, with timestamps shifted to start at 0.
[[nodiscard]] Status write_all_frames(MediaReader& reader, const GridSize& grid, Rgb background,
                                      const InterruptGuard& interrupt, VisualWriter& writer) {
    VideoFrame video;
    std::optional<std::int64_t> first_us;
    std::int64_t last_us = 0;
    for (;;) {
        if (interrupt.requested()) {
            return interrupted();
        }
        const Result<ReadStatus> read =
            reader.read_frame(grid.sample_w, grid.sample_h, video, background);
        const ReadStatus* status = read.value_if();
        if (status == nullptr) {
            return *read.error_if();
        }
        if (*status == ReadStatus::EndOfStream) {
            return Status::success();
        }
        const std::int64_t pts_us = video.pts_us;
        if (!first_us.has_value()) {
            first_us = pts_us;
        } else if (pts_us <= last_us) {
            return make_error(ErrorCode::DecodeError, "the input's frame times do not increase");
        } else if (*first_us < 0 &&
                   pts_us > std::numeric_limits<std::int64_t>::max() + *first_us) {
            return make_error(ErrorCode::DecodeError, "the input's frame times are out of range");
        }
        last_us = pts_us;
        if (Status s = writer.add(video, pts_us - *first_us); !s.ok()) {
            return s;
        }
    }
}

// Writes the whole export to `path`. The reader and the encoder are closed when
// this returns, so the caller can rename or remove the files.
[[nodiscard]] Status write_visual(const CliOptions& options, const std::filesystem::path& path,
                                  const InterruptGuard& interrupt) {
    const Result<GlyphAtlas> loaded = load_atlas(options);
    const GlyphAtlas* atlas = loaded.value_if();
    if (atlas == nullptr) {
        return *loaded.error_if();
    }
    ConvertConfig config = options.config;
    if (!options.aspect_given) {
        config.cell_aspect = atlas->cell_aspect();
    }
    if (Status s = validate(config); !s.ok()) {
        return s;
    }

    Result<MediaReader> opened = MediaReader::open(options.input);
    MediaReader* reader = opened.value_if();
    if (reader == nullptr) {
        return *opened.error_if();
    }
    const MediaInfo& info = reader->info();
    const bool still = options.format == OutputFormat::Png;
    if (!still && info.media_kind == MediaKind::Image) {
        return make_error(ErrorCode::InvalidArgument,
                          "gif and mp4 output need an animated or video input; use png");
    }
    const Result<GridSize> computed = compute_grid(info.src_width, info.src_height, config);
    const GridSize* grid = computed.value_if();
    if (grid == nullptr) {
        return *computed.error_if();
    }

    VisualWriter writer(path, options, config, *atlas);
    if (still) {
        VideoFrame video;
        if (Status s = read_frame_at(*reader, *grid, config.bg_color, options.frame_index,
                                     &interrupt, video);
            !s.ok()) {
            return s;
        }
        if (Status s = writer.add(video, 0); !s.ok()) {
            return s;
        }
    } else if (Status s = write_all_frames(*reader, *grid, config.bg_color, interrupt, writer);
               !s.ok()) {
        return s;
    }
    return writer.finish();
}

// Removes the file, if it is still there, on destruction.
class FileRemover {
public:
    explicit FileRemover(const std::filesystem::path& path) : path_(path) {}
    ~FileRemover() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);  // best effort: the export already has its status
    }
    FileRemover(const FileRemover&) = delete;
    FileRemover& operator=(const FileRemover&) = delete;

private:
    const std::filesystem::path& path_;
};

// Exports to a temporary file beside the output and renames it on success, so a
// failed or interrupted run leaves an existing output (or the input, when it is
// the output) as it was.
[[nodiscard]] Status export_visual(const CliOptions& options) {
    std::filesystem::path temp = options.output;
    temp += ".vglyph-tmp";
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::symlink_status(temp, ec))) {
        return make_error(ErrorCode::IoError,
                          utf8_string(temp) + " already exists; remove it and try again");
    }

    const Result<InterruptGuard> installed = InterruptGuard::install();
    const InterruptGuard* interrupt = installed.value_if();
    if (interrupt == nullptr) {
        return *installed.error_if();
    }
    const FileRemover remover(temp);
    const Status written = write_visual(options, temp, *interrupt);
    if (interrupt->requested()) {
        return interrupted();
    }
    if (!written.ok()) {
        return written;
    }
    std::filesystem::rename(temp, options.output, ec);
    if (ec) {
        return make_error(ErrorCode::IoError,
                          "cannot write " + utf8_string(options.output) + ": " + ec.message());
    }
    return Status::success();
}

[[nodiscard]] Status run_convert(const CliOptions& options) {
    if (is_visual(options.format)) {
        return export_visual(options);
    }
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
    if (const Status s = read_frame_at(*reader, *size, options.config.bg_color,
                                       options.frame_index, nullptr, video);
        !s.ok()) {
        return s;
    }

    AsciiFrame frame;
    if (const Status s = convert(video.rgb24, video.width, video.height, options.config, frame);
        !s.ok()) {
        return s;
    }

    const bool ansi = options.format == OutputFormat::Ans;
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
            result = run_play(*options);
            break;
    }
    if (!result.ok()) {
        // A user interrupt (Ctrl+C) is not an error worth printing.
        if (result.error().code != ErrorCode::Cancelled) {
            report(result.error());
        }
        return exit_code(result.error().code);
    }
    return 0;
}

}  // namespace vglyph::cli
