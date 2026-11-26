#pragma once

// FFmpeg-backed reader for photos, animated images and videos
// (SPEC sections 3.4, 5). The header exposes no FFmpeg types.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "vglyph/error.hpp"
#include "vglyph/frame.hpp"

namespace vglyph {

// Silences FFmpeg's own diagnostics (written to stderr by av_log). Errors
// still reach the caller through Error::detail, so an application that owns
// the terminal or must keep stderr clean calls this once at startup.
void set_media_log_quiet() noexcept;

enum class MediaKind { Image, AnimatedImage, Video };

struct MediaInfo {
    MediaKind media_kind = MediaKind::Image;
    std::uint32_t src_width = 0;
    std::uint32_t src_height = 0;
    std::string pixel_format_name;
    std::string codec_name;
    double avg_fps = 0.0;            // 0 for still images
    std::int64_t duration_us = 0;    // 0 if unknown or a still image
    bool has_audio = false;
    int audio_stream_index = -1;     // v2 passthrough hook; no audio decoder is opened
};

// Bound on the magnitude of every VideoFrame::pts_us and duration_us (about 71
// years), so that callers can add and subtract them, also as nanoseconds,
// without overflow. A frame outside it is a DecodeError.
inline constexpr std::int64_t kMaxFrameTimeUs = std::int64_t{1} << 51;

// One decoded frame, scaled to the requested size as tightly packed RGB24.
// The caller owns it and passes the same object to every read_frame() call:
// `rgb24` is resized only when the requested size changes.
struct VideoFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb24;
    std::int64_t pts_us = 0;       // relative to the stream start; 0 for still images
    std::int64_t duration_us = 0;  // 0 for still images
};

enum class ReadStatus { Frame, EndOfStream };

class MediaReader {
public:
    // Opens `path` and its best video stream. Fails with IoError (cannot open),
    // UnsupportedFormat (not media, no video stream, no decoder) or
    // LimitExceeded (source larger than the SPEC 5.3 caps).
    [[nodiscard]] static Result<MediaReader> open(const std::filesystem::path& path);

    MediaReader(MediaReader&&) noexcept;
    MediaReader& operator=(MediaReader&&) noexcept;
    MediaReader(const MediaReader&) = delete;
    MediaReader& operator=(const MediaReader&) = delete;
    ~MediaReader();

    [[nodiscard]] const MediaInfo& info() const noexcept;

    // Decodes the next frame and scales it (SWS_AREA) to width x height, which
    // is normally GridSize::sample_w x sample_h. Returns EndOfStream, and keeps
    // returning it, once every frame was read. A zero or oversized width/height
    // is InvalidArgument and consumes no frame. A frame time outside
    // kMaxFrameTimeUs is a DecodeError.
    //
    // A source with an alpha channel (PNG with transparency, GIF with a
    // transparent index, ...) is composited over `background` at the source
    // size, before scaling, so the hidden color of transparent pixels never
    // bleeds into the scaled result. Opaque sources ignore `background`.
    [[nodiscard]] Result<ReadStatus> read_frame(std::uint32_t width, std::uint32_t height,
                                                VideoFrame& out, Rgb background);

    // Repositions so that the next read_frame() returns the frame covering
    // `target_us` (pts <= target < pts + duration), or EndOfStream when the
    // target is past the last frame. Seeks to the nearest earlier keyframe,
    // then decodes forward without scaling the skipped frames. Where the
    // container cannot seek (still images, some formats), or its seek does not
    // lead to a keyframe at or before the target (MPEG-TS), it reopens the file
    // and decodes forward from the start, which takes longer the later the
    // target is. A still image has one frame at 0.
    // A negative target is InvalidArgument.
    [[nodiscard]] Status seek_us(std::int64_t target_us);

private:
    struct Impl;
    explicit MediaReader(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vglyph
