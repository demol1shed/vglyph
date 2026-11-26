#include "vglyph/media_exporter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "checked_math.hpp"
#include "ffmpeg_error.hpp"
#include "ffmpeg_raii.hpp"
#include "quantize.hpp"
#include "vglyph/convert.hpp"

namespace vglyph {

namespace {

constexpr std::size_t kBytesPerPixel = 4;
constexpr AVRational kMicros{1, 1'000'000};
constexpr AVRational kCentis{1, 100};  // GIF delays are in hundredths of a second

// H.264 yuv420p needs even sizes: an odd side gets one extra pixel of background.
[[nodiscard]] std::uint32_t round_up_even(std::uint32_t v) noexcept { return v + (v & 1u); }

[[nodiscard]] Status check_output_size(std::uint32_t width, std::uint32_t height) {
    std::uint64_t pixels = 0;
    if (width == 0 || height == 0) {
        return make_error(ErrorCode::InvalidArgument, "output size must not be zero");
    }
    if (width > kMaxSourceDim || height > kMaxSourceDim ||
        !checked_mul(width, height, pixels) || pixels > kMaxSourcePixels) {
        return make_error(ErrorCode::LimitExceeded,
                          "output is " + std::to_string(width) + "x" + std::to_string(height) +
                              ", over the " + std::to_string(kMaxSourceDim) + " per axis / " +
                              std::to_string(kMaxSourcePixels) + " pixel cap");
    }
    return Status::success();
}

}  // namespace

struct MediaExporter::Impl {
    ExportFormat format = ExportFormat::Png;
    ExportOptions options;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ffmpeg::OutputFormatCtx oc;
    AVStream* stream = nullptr;  // owned by `oc`
    ffmpeg::CodecCtx enc;
    ffmpeg::FramePtr frame;
    ffmpeg::PacketPtr pkt;
    std::uint64_t frames_written = 0;
    bool finished = false;

    // GIF only.
    ffmpeg::FramePtr spare_frame;  // the encoder keeps the last frame, so two take turns
    std::unique_ptr<Quantizer> quantizer;
    std::vector<std::uint8_t> gif_indices;            // width * height palette indices
    std::array<std::uint32_t, kPaletteSize> palette{};
    std::int64_t last_pts_cs = -1;                    // keeps frame times strictly increasing
    std::int64_t last_duration_cs = 0;                // the packet of the frame just encoded

    // MP4 only.
    ffmpeg::SwsPtr sws;                  // RGBA -> yuv420p at the padded size
    std::vector<std::uint8_t> padded;    // RGBA at the padded size; empty when no padding is needed
    std::uint32_t padded_width = 0;
    std::uint32_t padded_height = 0;
    std::int64_t last_pts_us = -1;
    // libx264 does not copy a frame's duration to its packet, and its packets lag
    // behind the frames, so durations wait here (keyed by pts) until their packet
    // appears. 256 slots outlast the encoder's delay (lookahead 40 plus threads).
    struct PendingDuration {
        std::int64_t pts = -1;
        std::int64_t duration_us = 0;
    };
    std::array<PendingDuration, 256> pending{};
    std::uint64_t pending_count = 0;

    [[nodiscard]] Status open(const std::filesystem::path& path);
    [[nodiscard]] Status write_frame(const RgbaImage& image, std::int64_t pts_us,
                                     std::int64_t duration_us);
    [[nodiscard]] Status finish();

private:
    // Sends `f` (null flushes) and writes every packet the encoder returns.
    [[nodiscard]] Status encode(const AVFrame* f);
    // Copies `image` into the (writable) frame, quantizing (GIF) or converting (MP4) it.
    [[nodiscard]] Status fill_frame(const RgbaImage& image);
    // Sets frame->pts and frame->duration (and the GIF/MP4 timeline state).
    [[nodiscard]] Status place_frame(std::int64_t pts_us, std::int64_t duration_us);
};

Status MediaExporter::Impl::open(const std::filesystem::path& path) {
    if (Status size = check_output_size(width, height); !size.ok()) {
        return size;
    }

    const char* muxer = nullptr;
    const AVCodec* codec = nullptr;
    AVPixelFormat pix_fmt = AV_PIX_FMT_RGBA;
    AVRational time_base{1, 1};
    switch (format) {
        case ExportFormat::Png:
            muxer = "image2";
            codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
            break;
        case ExportFormat::Gif:
            muxer = "gif";
            codec = avcodec_find_encoder(AV_CODEC_ID_GIF);
            pix_fmt = AV_PIX_FMT_PAL8;
            time_base = kCentis;
            break;
        case ExportFormat::Mp4:
            if (options.crf < kMinCrf || options.crf > kMaxCrf) {
                return make_error(ErrorCode::InvalidArgument,
                                  "crf must be " + std::to_string(kMinCrf) + ".." +
                                      std::to_string(kMaxCrf));
            }
            muxer = "mp4";
            codec = avcodec_find_encoder_by_name("libx264");
            pix_fmt = AV_PIX_FMT_YUV420P;
            time_base = kMicros;  // keeps variable frame rates exact
            break;
    }
    if (codec == nullptr) {
        return make_error(ErrorCode::UnsupportedFormat,
                          "this FFmpeg build has no encoder for the format");
    }

    const std::u8string u8 = path.u8string();
    // Without "file:", FFmpeg writes "a:b.png" or "http://..." through that protocol.
    const std::string name =
        "file:" + std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    AVFormatContext* raw_oc = nullptr;
    int rc = avformat_alloc_output_context2(&raw_oc, nullptr, muxer, name.c_str());
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::UnsupportedFormat, "avformat_alloc_output_context2", rc);
    }
    oc.reset(raw_oc);

    stream = avformat_new_stream(oc.get(), nullptr);
    enc.reset(avcodec_alloc_context3(codec));
    if (stream == nullptr || !enc) {
        return make_error(ErrorCode::InternalError, "cannot allocate the stream or encoder");
    }
    padded_width = format == ExportFormat::Mp4 ? round_up_even(width) : width;
    padded_height = format == ExportFormat::Mp4 ? round_up_even(height) : height;
    enc->width = static_cast<int>(padded_width);
    enc->height = static_cast<int>(padded_height);
    enc->pix_fmt = pix_fmt;
    enc->time_base = time_base;
    if ((oc->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (format == ExportFormat::Mp4) {
        enc->colorspace = AVCOL_SPC_BT470BG;  // what sws_scale uses by default (BT.601, limited range)
        enc->color_range = AVCOL_RANGE_MPEG;
        // Without B-frames dts equals pts, so the muxer derives each frame's duration
        // from the pts gaps and a variable frame rate survives a read-back exactly.
        enc->max_b_frames = 0;
        rc = av_opt_set_int(enc->priv_data, "crf", options.crf, 0);
        if (rc < 0) {
            return ffmpeg_error(ErrorCode::EncodeError, "av_opt_set_int(crf)", rc);
        }
    }
    rc = avcodec_open2(enc.get(), codec, nullptr);
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::EncodeError, "avcodec_open2", rc);
    }
    rc = avcodec_parameters_from_context(stream->codecpar, enc.get());
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::InternalError, "avcodec_parameters_from_context", rc);
    }
    stream->time_base = enc->time_base;

    frame.reset(av_frame_alloc());
    pkt.reset(av_packet_alloc());
    if (!frame || !pkt) {
        return make_error(ErrorCode::InternalError, "cannot allocate the frame or packet");
    }
    frame->format = enc->pix_fmt;
    frame->width = enc->width;
    frame->height = enc->height;
    rc = av_frame_get_buffer(frame.get(), 0);
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::InternalError, "av_frame_get_buffer", rc);
    }
    if (format == ExportFormat::Gif) {
        spare_frame.reset(av_frame_clone(frame.get()));
        if (!spare_frame) {
            return make_error(ErrorCode::InternalError, "av_frame_clone failed");
        }
        rc = av_frame_make_writable(spare_frame.get());  // gives the clone its own buffer
        if (rc < 0) {
            return ffmpeg_error(ErrorCode::InternalError, "av_frame_make_writable", rc);
        }
        quantizer = std::make_unique<Quantizer>();
        gif_indices.resize(static_cast<std::size_t>(width) * height);
    }
    if (format == ExportFormat::Mp4) {
        sws.reset(sws_getContext(enc->width, enc->height, AV_PIX_FMT_RGBA, enc->width, enc->height,
                                 AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws) {
            return make_error(ErrorCode::UnsupportedFormat, "cannot convert RGBA to yuv420p");
        }
        if (padded_width != width || padded_height != height) {
            padded.resize(static_cast<std::size_t>(padded_width) * padded_height * kBytesPerPixel);
            for (std::size_t i = 0; i < padded.size(); i += kBytesPerPixel) {
                padded[i] = options.bg_color.r;
                padded[i + 1] = options.bg_color.g;
                padded[i + 2] = options.bg_color.b;
                padded[i + 3] = 255;
            }
        }
    }

    rc = avio_open(&oc->pb, name.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::IoError, "avio_open", rc);
    }
    // The image2 muxer expects a "%d" pattern unless told it writes one image.
    AVDictionary* raw_opts = nullptr;
    if (format == ExportFormat::Mp4) {
        rc = av_dict_set(&raw_opts, "movflags", "+faststart", 0);  // moov atom first, for streaming
        if (rc < 0) {
            const ffmpeg::DictPtr cleanup(raw_opts);
            return ffmpeg_error(ErrorCode::InternalError, "av_dict_set", rc);
        }
    } else if (format == ExportFormat::Png) {
        rc = av_dict_set(&raw_opts, "update", "1", 0);
        if (rc < 0) {
            const ffmpeg::DictPtr cleanup(raw_opts);
            return ffmpeg_error(ErrorCode::InternalError, "av_dict_set", rc);
        }
    }
    rc = avformat_write_header(oc.get(), &raw_opts);
    const ffmpeg::DictPtr leftover_opts(raw_opts);
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::EncodeError, "avformat_write_header", rc);
    }
    return Status::success();
}

Status MediaExporter::Impl::encode(const AVFrame* f) {
    int rc = avcodec_send_frame(enc.get(), f);
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::EncodeError, "avcodec_send_frame", rc);
    }
    for (;;) {
        rc = avcodec_receive_packet(enc.get(), pkt.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return Status::success();
        }
        if (rc < 0) {
            return ffmpeg_error(ErrorCode::EncodeError, "avcodec_receive_packet", rc);
        }
        pkt->stream_index = stream->index;
        if (format == ExportFormat::Gif) {
            pkt->duration = last_duration_cs;  // no reordering: this packet is the last frame
        } else if (format == ExportFormat::Mp4) {
            for (const PendingDuration& p : pending) {
                if (p.pts == pkt->pts) {
                    pkt->duration = p.duration_us;
                    break;
                }
            }
        }
        av_packet_rescale_ts(pkt.get(), enc->time_base, stream->time_base);
        rc = av_interleaved_write_frame(oc.get(), pkt.get());  // unrefs the packet, even on error
        if (rc < 0) {
            return ffmpeg_error(ErrorCode::IoError, "av_interleaved_write_frame", rc);
        }
    }
}

Status MediaExporter::Impl::place_frame(std::int64_t pts_us, std::int64_t duration_us) {
    if (format == ExportFormat::Png) {
        frame->pts = 0;  // a PNG has no timeline
        return Status::success();
    }
    if (pts_us > std::numeric_limits<std::int64_t>::max() - duration_us) {
        return make_error(ErrorCode::InvalidArgument, "pts_us + duration_us is out of range");
    }
    if (format == ExportFormat::Mp4) {
        if (pts_us <= last_pts_us) {
            return make_error(ErrorCode::InvalidArgument, "frame times must increase");
        }
        last_pts_us = pts_us;
        pending[pending_count % pending.size()] = PendingDuration{pts_us, duration_us};
        ++pending_count;
        frame->pts = pts_us;
        frame->duration = duration_us;
        return Status::success();
    }
    // GIF: round the cumulative times, not each duration, so rounding never drifts.
    const std::int64_t start = av_rescale_q(pts_us, kMicros, kCentis);
    const std::int64_t end = av_rescale_q(pts_us + duration_us, kMicros, kCentis);
    const std::int64_t pts = std::max(start, last_pts_cs + 1);
    last_pts_cs = pts;
    last_duration_cs = std::max<std::int64_t>(1, end - pts);  // a 0 cs delay would mean "default"
    frame->pts = pts;
    frame->duration = last_duration_cs;
    return Status::success();
}

Status MediaExporter::Impl::fill_frame(const RgbaImage& image) {
    const std::size_t w = width;
    if (format == ExportFormat::Gif) {
        quantizer->quantize(image.rgba.data(), w * height, gif_indices.data(), palette.data());
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0],
                        gif_indices.data() + y * w, w);
        }
        std::memcpy(frame->data[1], palette.data(), sizeof(std::uint32_t) * palette.size());
        return Status::success();
    }
    const std::size_t row_bytes = w * kBytesPerPixel;
    if (format == ExportFormat::Mp4) {
        const std::uint8_t* src = image.rgba.data();
        std::size_t src_stride = row_bytes;
        if (!padded.empty()) {  // the extra column and row keep the background fill from open()
            src_stride = static_cast<std::size_t>(padded_width) * kBytesPerPixel;
            for (std::uint32_t y = 0; y < height; ++y) {
                std::memcpy(padded.data() + y * src_stride, image.rgba.data() + y * row_bytes,
                            row_bytes);
            }
            src = padded.data();
        }
        const std::array<const std::uint8_t*, 4> src_data{src, nullptr, nullptr, nullptr};
        const std::array<int, 4> src_linesize{static_cast<int>(src_stride), 0, 0, 0};
        const int rows = sws_scale(sws.get(), src_data.data(), src_linesize.data(), 0,
                                   static_cast<int>(padded_height), frame->data, frame->linesize);
        if (rows != static_cast<int>(padded_height)) {
            return make_error(ErrorCode::EncodeError, "sws_scale produced " + std::to_string(rows) +
                                                          " rows, expected " +
                                                          std::to_string(padded_height));
        }
        return Status::success();
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0],
                    image.rgba.data() + y * row_bytes, row_bytes);
    }
    return Status::success();
}

Status MediaExporter::Impl::write_frame(const RgbaImage& image, std::int64_t pts_us,
                                        std::int64_t duration_us) {
    if (finished) {
        return make_error(ErrorCode::InvalidArgument, "the exporter was already finished");
    }
    if (image.width != width || image.height != height ||
        image.rgba.size() != static_cast<std::size_t>(width) * height * kBytesPerPixel) {
        return make_error(ErrorCode::InvalidArgument,
                          "frame is not " + std::to_string(width) + "x" + std::to_string(height));
    }
    if (pts_us < 0 || duration_us < 0) {
        return make_error(ErrorCode::InvalidArgument, "pts_us and duration_us must not be negative");
    }
    if (format == ExportFormat::Png && frames_written > 0) {
        return make_error(ErrorCode::InvalidArgument, "a PNG holds exactly one frame");
    }

    // The GIF encoder still references the frame it was given last; writing into
    // that one would make FFmpeg allocate a fresh buffer on every call.
    if (spare_frame && av_frame_is_writable(frame.get()) == 0) {
        std::swap(frame, spare_frame);
    }
    int rc = av_frame_make_writable(frame.get());
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::InternalError, "av_frame_make_writable", rc);
    }
    if (Status placed = place_frame(pts_us, duration_us); !placed.ok()) {
        return placed;
    }
    if (Status filled = fill_frame(image); !filled.ok()) {
        return filled;
    }
    if (Status sent = encode(frame.get()); !sent.ok()) {
        return sent;
    }
    ++frames_written;
    return Status::success();
}

Status MediaExporter::Impl::finish() {
    if (finished) {
        return make_error(ErrorCode::InvalidArgument, "the exporter was already finished");
    }
    if (frames_written == 0) {
        return make_error(ErrorCode::InvalidArgument, "no frame was written");
    }
    finished = true;
    if (Status flushed = encode(nullptr); !flushed.ok()) {
        return flushed;
    }
    int rc = av_write_trailer(oc.get());
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::IoError, "av_write_trailer", rc);
    }
    rc = avio_closep(&oc->pb);
    if (rc < 0) {
        return ffmpeg_error(ErrorCode::IoError, "avio_closep", rc);
    }
    return Status::success();
}

MediaExporter::MediaExporter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
MediaExporter::MediaExporter(MediaExporter&&) noexcept = default;
MediaExporter& MediaExporter::operator=(MediaExporter&&) noexcept = default;
MediaExporter::~MediaExporter() = default;

Result<MediaExporter> MediaExporter::open(const std::filesystem::path& path, ExportFormat format,
                                          std::uint32_t width, std::uint32_t height,
                                          const ExportOptions& options) {
    auto impl = std::make_unique<Impl>();
    impl->format = format;
    impl->options = options;
    impl->width = width;
    impl->height = height;
    if (Status opened = impl->open(path); !opened.ok()) {
        return opened.error();
    }
    return MediaExporter(std::move(impl));
}

Status MediaExporter::write_frame(const RgbaImage& image, std::int64_t pts_us,
                                  std::int64_t duration_us) {
    return impl_->write_frame(image, pts_us, duration_us);
}

Status MediaExporter::finish() { return impl_->finish(); }

}  // namespace vglyph
