#include "vglyph/media_reader.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "checked_math.hpp"
#include "ffmpeg_error.hpp"
#include "ffmpeg_raii.hpp"
#include "vglyph/convert.hpp"

namespace vglyph {

namespace {

constexpr AVRational kMicros{1, 1'000'000};

// SPEC section 5.3 caps on a source frame.
[[nodiscard]] Status check_source_size(int width, int height) {
    if (width <= 0 || height <= 0) {
        return make_error(ErrorCode::LimitExceeded, "source dimensions must be positive");
    }
    const auto w = static_cast<std::uint32_t>(width);
    const auto h = static_cast<std::uint32_t>(height);
    std::uint64_t pixels = 0;
    if (w > kMaxSourceDim || h > kMaxSourceDim || !checked_mul(w, h, pixels) ||
        pixels > kMaxSourcePixels) {
        return make_error(ErrorCode::LimitExceeded,
                          "source is " + std::to_string(w) + "x" + std::to_string(h) +
                              ", over the " + std::to_string(kMaxSourceDim) + " per axis / " +
                              std::to_string(kMaxSourcePixels) + " pixel cap");
    }
    return Status::success();
}

// Failures that mean "this is not something we can read", as opposed to I/O.
[[nodiscard]] ErrorCode open_error_code(int averr) noexcept {
    const bool unsupported = averr == AVERROR_INVALIDDATA || averr == AVERROR_STREAM_NOT_FOUND ||
                             averr == AVERROR_DEMUXER_NOT_FOUND ||
                             averr == AVERROR_DECODER_NOT_FOUND;
    return unsupported ? ErrorCode::UnsupportedFormat : ErrorCode::IoError;
}

[[nodiscard]] MediaKind classify(std::string_view demuxer) noexcept {
    if (demuxer == "gif" || demuxer == "apng") {
        return MediaKind::AnimatedImage;
    }
    if (demuxer == "image2" || demuxer.ends_with("_pipe")) {
        return MediaKind::Image;
    }
    return MediaKind::Video;
}

[[nodiscard]] bool valid_rate(AVRational r) noexcept { return r.num > 0 && r.den > 0; }

}  // namespace

struct MediaReader::Impl {
    ffmpeg::FormatCtx fmt;
    ffmpeg::CodecCtx dec;
    ffmpeg::FramePtr frame;
    ffmpeg::PacketPtr pkt;
    ffmpeg::SwsPtr sws;
    MediaInfo info;
    int video_index = -1;
    AVRational time_base{1, 1};
    std::int64_t start_ts = 0;             // stream start, in time_base units
    std::int64_t fallback_duration_us = 0; // 1 / frame rate, for frames without a duration
    std::int64_t next_pts_us = 0;          // end of the previous frame
    bool draining = false;                 // the decoder was told the input ended

    [[nodiscard]] Status open(const std::filesystem::path& path);
    [[nodiscard]] Result<ReadStatus> read_frame(std::uint32_t width, std::uint32_t height,
                                                VideoFrame& out);

private:
    [[nodiscard]] std::int64_t to_us(std::int64_t ts) const noexcept {
        return av_rescale_q(ts, time_base, kMicros);
    }
    [[nodiscard]] Result<ReadStatus> emit_frame(std::uint32_t width, std::uint32_t height,
                                                std::uint64_t bytes, VideoFrame& out);
};

Status MediaReader::Impl::open(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    const std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());

    // GIF delays are in centiseconds; FFmpeg replaces delays below min_delay
    // with default_delay. min_delay=1 limits that to the 0 cs quirk, giving
    // SPEC 5.2's "sub-10 ms becomes 100 ms". Other demuxers ignore the option.
    AVDictionary* raw_opts = nullptr;
    int rc = av_dict_set(&raw_opts, "min_delay", "1", 0);
    if (rc < 0) {
        const ffmpeg::DictPtr cleanup(raw_opts);
        return ffmpeg_error(ErrorCode::InternalError, "av_dict_set", rc);
    }
    AVFormatContext* raw_fmt = nullptr;
    rc = avformat_open_input(&raw_fmt, name.c_str(), nullptr, &raw_opts);
    const ffmpeg::DictPtr leftover_opts(raw_opts);
    if (rc < 0) {
        return ffmpeg_error(open_error_code(rc), "avformat_open_input", rc);
    }
    fmt.reset(raw_fmt);

    rc = avformat_find_stream_info(fmt.get(), nullptr);
    if (rc < 0) {
        return ffmpeg_error(open_error_code(rc), "avformat_find_stream_info", rc);
    }

    const AVCodec* codec = nullptr;
    video_index = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (video_index < 0) {
        return ffmpeg_error(ErrorCode::UnsupportedFormat, "no decodable video stream", video_index);
    }
    const AVStream* stream = fmt->streams[video_index];
    const AVCodecParameters* par = stream->codecpar;
    if (Status size = check_source_size(par->width, par->height); !size.ok()) {
        return size;
    }

    info.media_kind = classify(fmt->iformat->name);
    info.src_width = static_cast<std::uint32_t>(par->width);
    info.src_height = static_cast<std::uint32_t>(par->height);
    const char* pix_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format));
    info.pixel_format_name = pix_name != nullptr ? pix_name : "unknown";
    info.codec_name = avcodec_get_name(par->codec_id);

    time_base = stream->time_base;
    start_ts = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    if (info.media_kind != MediaKind::Image) {
        const AVRational rate = valid_rate(stream->avg_frame_rate) ? stream->avg_frame_rate
                                                                   : stream->r_frame_rate;
        if (valid_rate(rate)) {
            info.avg_fps = av_q2d(rate);
            fallback_duration_us = av_rescale_q(1, av_inv_q(rate), kMicros);
        }
        if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
            info.duration_us = fmt->duration;
        }
    }

    // Audio is never decoded in v1; remember where it is for a later passthrough.
    const int audio_index = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, video_index,
                                                nullptr, 0);
    info.has_audio = audio_index >= 0;
    info.audio_stream_index = audio_index >= 0 ? audio_index : -1;

    dec.reset(avcodec_alloc_context3(codec));
    if (!dec) {
        return make_error(ErrorCode::InternalError, "avcodec_alloc_context3 failed");
    }
    rc = avcodec_parameters_to_context(dec.get(), par);
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::InternalError, "avcodec_parameters_to_context", rc);
    }
    dec->pkt_timebase = time_base;
    if (info.media_kind == MediaKind::Video) {
        dec->thread_count = 0;  // let FFmpeg pick; stills and GIFs gain nothing from threads
    }
    rc = avcodec_open2(dec.get(), codec, nullptr);
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::UnsupportedFormat, "avcodec_open2", rc);
    }

    frame.reset(av_frame_alloc());
    pkt.reset(av_packet_alloc());
    if (!frame || !pkt) {
        return make_error(ErrorCode::InternalError, "cannot allocate the frame or packet");
    }
    return Status::success();
}

Result<ReadStatus> MediaReader::Impl::read_frame(std::uint32_t width, std::uint32_t height,
                                                 VideoFrame& out) {
    if (width == 0 || height == 0 || width > kMaxSourceDim || height > kMaxSourceDim) {
        return make_error(ErrorCode::InvalidArgument,
                          "target size must be 1.." + std::to_string(kMaxSourceDim) + " per axis");
    }
    std::uint64_t pixels = 0;
    std::uint64_t bytes = 0;
    if (!checked_mul(width, height, pixels) || pixels > kMaxSourcePixels ||
        !checked_mul(pixels, 3, bytes)) {
        return make_error(ErrorCode::LimitExceeded, "target size is over the pixel cap");
    }

    for (;;) {
        int rc = avcodec_receive_frame(dec.get(), frame.get());
        if (rc == 0) {
            return emit_frame(width, height, bytes, out);
        }
        if (rc == AVERROR_EOF) {
            return ReadStatus::EndOfStream;
        }
        if (rc != AVERROR(EAGAIN)) {
            return ffmpeg_error(ErrorCode::DecodeError, "avcodec_receive_frame", rc);
        }

        // The decoder wants more input.
        if (draining) {
            return ReadStatus::EndOfStream;
        }
        rc = av_read_frame(fmt.get(), pkt.get());
        if (rc == AVERROR_EOF) {
            rc = avcodec_send_packet(dec.get(), nullptr);  // start draining
            if (rc < 0) {
                return ffmpeg_error(ErrorCode::DecodeError, "avcodec_send_packet (flush)", rc);
            }
            draining = true;
            continue;
        }
        if (rc < 0) {
            return ffmpeg_error(ErrorCode::IoError, "av_read_frame", rc);
        }
        if (pkt->stream_index != video_index) {
            av_packet_unref(pkt.get());  // audio and other streams are skipped
            continue;
        }
        rc = avcodec_send_packet(dec.get(), pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) {
            // EAGAIN is a bug here too: receive_frame just said it wants input.
            return ffmpeg_error(ErrorCode::DecodeError, "avcodec_send_packet", rc);
        }
    }
}

Result<ReadStatus> MediaReader::Impl::emit_frame(std::uint32_t width, std::uint32_t height,
                                                  std::uint64_t bytes, VideoFrame& out) {
    const ffmpeg::FrameUnref release(frame.get());
    if (Status size = check_source_size(frame->width, frame->height); !size.ok()) {
        return size.error();  // the stream changed size mid-way
    }

    // Reuses the context while the source and target sizes stay the same.
    SwsContext* scaler = sws_getCachedContext(
        sws.release(), frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_RGB24, SWS_AREA, nullptr,
        nullptr, nullptr);
    sws.reset(scaler);
    if (!sws) {
        return make_error(ErrorCode::UnsupportedFormat, "cannot scale this pixel format to RGB24");
    }

    if (out.width != width || out.height != height) {
        out.rgb24.resize(static_cast<std::size_t>(bytes));
        out.width = width;
        out.height = height;
    }
    const std::array<std::uint8_t*, 4> dst{out.rgb24.data(), nullptr, nullptr, nullptr};
    const std::array<int, 4> dst_stride{static_cast<int>(width * 3), 0, 0, 0};
    const int rows = sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height,
                               dst.data(), dst_stride.data());
    if (rows != static_cast<int>(height)) {
        return make_error(ErrorCode::DecodeError, "sws_scale produced " + std::to_string(rows) +
                                                      " rows, expected " + std::to_string(height));
    }

    std::int64_t pts_us = next_pts_us;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        pts_us = to_us(frame->best_effort_timestamp - start_ts);
    }
    std::int64_t duration_us = frame->duration > 0 ? to_us(frame->duration) : fallback_duration_us;
    if (info.media_kind == MediaKind::Image) {
        pts_us = 0;
        duration_us = 0;
    }
    next_pts_us = pts_us + duration_us;
    out.pts_us = pts_us;
    out.duration_us = duration_us;
    return ReadStatus::Frame;
}

MediaReader::MediaReader(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
MediaReader::MediaReader(MediaReader&&) noexcept = default;
MediaReader& MediaReader::operator=(MediaReader&&) noexcept = default;
MediaReader::~MediaReader() = default;

Result<MediaReader> MediaReader::open(const std::filesystem::path& path) {
    auto impl = std::make_unique<Impl>();
    if (Status opened = impl->open(path); !opened.ok()) {
        return opened.error();
    }
    return MediaReader(std::move(impl));
}

const MediaInfo& MediaReader::info() const noexcept { return impl_->info; }

Result<ReadStatus> MediaReader::read_frame(std::uint32_t width, std::uint32_t height,
                                           VideoFrame& out) {
    return impl_->read_frame(width, height, out);
}

}  // namespace vglyph
