#pragma once

// FFmpeg-backed writer for visual export: one PNG, an animated GIF or an H.264
// MP4 (SPEC section 7.2). The header exposes no FFmpeg types.

#include <cstdint>
#include <filesystem>
#include <memory>

#include "vglyph/error.hpp"
#include "vglyph/frame.hpp"
#include "vglyph/rasterizer.hpp"

namespace vglyph {

enum class ExportFormat { Png, Gif, Mp4 };

// Accepted `crf` range of the H.264 encoder.
inline constexpr int kMinCrf = 0;
inline constexpr int kMaxCrf = 51;

struct ExportOptions {
    int crf = 20;          // MP4 only: H.264 quality, 0 (lossless) to 51 (worst)
    Rgb bg_color{0, 0, 0}; // MP4 only: fills the pixel added to an odd width or height
};

class MediaExporter {
public:
    // Creates `path` and prepares to write frames of exactly width x height
    // pixels. Fails with InvalidArgument (zero size, crf out of range),
    // LimitExceeded (over kMaxSourceDim per axis or kMaxSourcePixels),
    // UnsupportedFormat (this FFmpeg build lacks the encoder) or IoError
    // (the file cannot be created). open() creates the file; after any later
    // error the caller removes the incomplete file.
    [[nodiscard]] static Result<MediaExporter> open(const std::filesystem::path& path,
                                                    ExportFormat format, std::uint32_t width,
                                                    std::uint32_t height,
                                                    const ExportOptions& options = {});

    MediaExporter(MediaExporter&&) noexcept;
    MediaExporter& operator=(MediaExporter&&) noexcept;
    MediaExporter(const MediaExporter&) = delete;
    MediaExporter& operator=(const MediaExporter&) = delete;
    ~MediaExporter();

    // Encodes one frame. `image` must be width x height as given to open().
    // `pts_us` and `duration_us` (microseconds) place the frame on the
    // timeline; PNG ignores both. A PNG takes one frame: a second call is
    // InvalidArgument. Calling this after finish() is InvalidArgument too.
    [[nodiscard]] Status write_frame(const RgbaImage& image, std::int64_t pts_us,
                                     std::int64_t duration_us);

    // Flushes the encoder and completes the file. Without at least one frame
    // this is InvalidArgument. Dropping the exporter without calling finish()
    // leaves an incomplete file.
    [[nodiscard]] Status finish();

private:
    struct Impl;
    explicit MediaExporter(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vglyph
