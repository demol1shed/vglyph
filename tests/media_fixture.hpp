#pragma once

// Test-only writers for tiny PNG, GIF and MP4 files. Phase 4's MediaExporter
// will replace them. PNG and MP4 go through FFmpeg's own encoders (no x264);
// the GIF is written by hand so every frame delay is exact.

#include <algorithm>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "ffmpeg_error.hpp"
#include "ffmpeg_raii.hpp"
#include "vglyph/error.hpp"
#include "vglyph/frame.hpp"

namespace vglyph_test {

namespace fs = std::filesystem;

// A fresh directory under the system temp dir, removed on destruction.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        if (ec) {
            return;
        }
        path_ = base / ("vglyph-test-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                        "-" + std::to_string(counter++));
        fs::create_directories(path_, ec);
        if (ec) {
            path_.clear();
        }
    }
    ~TempDir() {
        if (!path_.empty()) {
            std::error_code ec;
            (void)fs::remove_all(path_, ec);  // best-effort cleanup of a temp dir
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] bool ok() const { return !path_.empty(); }
    [[nodiscard]] fs::path file(const char* name) const { return path_ / name; }

private:
    fs::path path_;
};

[[nodiscard]] inline bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return !out.fail();
}

[[nodiscard]] inline bool read_file(const fs::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// ---- RGB24 pixel helpers ---------------------------------------------------

[[nodiscard]] inline std::vector<std::uint8_t> solid_rgb(int w, int h, vglyph::Rgb c) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    for (std::size_t i = 0; i < px.size(); i += 3) {
        px[i] = c.r;
        px[i + 1] = c.g;
        px[i + 2] = c.b;
    }
    return px;
}

// Top-left, top-right, bottom-left, bottom-right quadrants (w and h even).
[[nodiscard]] inline std::vector<std::uint8_t> quadrants_rgb(int w, int h,
                                                             const std::array<vglyph::Rgb, 4>& q) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const vglyph::Rgb c = q[static_cast<std::size_t>((y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0))];
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                   static_cast<std::size_t>(x)) * 3;
            px[i] = c.r;
            px[i + 1] = c.g;
            px[i + 2] = c.b;
        }
    }
    return px;
}

// ---- FFmpeg-encoded files (PNG, MP4) -----------------------------------------

struct FixtureFrame {
    std::vector<std::uint8_t> rgb24;  // RGBA bytes when the spec's pix_fmt is AV_PIX_FMT_RGBA
    std::int64_t pts = 0;       // in spec.time_base ticks
    std::int64_t duration = 0;  // in spec.time_base ticks
};

struct FixtureSpec {
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    bool single_image = false;  // image2 muxer writing one plain file name
    int width = 0;
    int height = 0;
    AVRational time_base{1, 25};
};

namespace detail {

struct OutputFormatDeleter {
    void operator()(AVFormatContext* p) const noexcept {
        if (p == nullptr) {
            return;
        }
        if ((p->oformat->flags & AVFMT_NOFILE) == 0) {
            (void)avio_closep(&p->pb);  // no-op after a successful explicit close
        }
        avformat_free_context(p);
    }
};
using OutputFormat = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;

[[nodiscard]] inline vglyph::Error fail(const char* what, int averr) {
    return vglyph::ffmpeg_error(vglyph::ErrorCode::EncodeError, what, averr);
}

}  // namespace detail

[[nodiscard]] inline vglyph::Status write_ffmpeg_fixture(const fs::path& path,
                                                         const FixtureSpec& spec,
                                                         const std::vector<FixtureFrame>& frames) {
    using namespace vglyph;
    const std::string name = path.string();

    AVFormatContext* raw_oc = nullptr;
    int rc = avformat_alloc_output_context2(&raw_oc, nullptr, spec.single_image ? "image2" : nullptr,
                                            name.c_str());
    if (rc < 0) {
        return detail::fail("avformat_alloc_output_context2", rc);
    }
    const detail::OutputFormat oc(raw_oc);

    const AVCodec* codec = avcodec_find_encoder(spec.codec_id);
    AVStream* st = avformat_new_stream(oc.get(), nullptr);
    ffmpeg::CodecCtx enc(codec != nullptr ? avcodec_alloc_context3(codec) : nullptr);
    if (codec == nullptr || st == nullptr || !enc) {
        return make_error(ErrorCode::EncodeError, "encoder or stream not available");
    }
    enc->width = spec.width;
    enc->height = spec.height;
    enc->pix_fmt = spec.pix_fmt;
    enc->time_base = spec.time_base;
    if (spec.codec_id == AV_CODEC_ID_MPEG4) {
        enc->gop_size = 2;
        enc->max_b_frames = 0;  // output order == input order, so pts == dts
        enc->flags |= AV_CODEC_FLAG_QSCALE;
        enc->global_quality = FF_QP2LAMBDA * 2;
    }
    if ((oc->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    rc = avcodec_open2(enc.get(), codec, nullptr);
    if (rc < 0) {
        return detail::fail("avcodec_open2", rc);
    }
    rc = avcodec_parameters_from_context(st->codecpar, enc.get());
    if (rc < 0) {
        return detail::fail("avcodec_parameters_from_context", rc);
    }
    st->time_base = spec.time_base;

    if ((oc->oformat->flags & AVFMT_NOFILE) == 0) {
        rc = avio_open(&oc->pb, name.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            return detail::fail("avio_open", rc);
        }
    }
    AVDictionary* raw_opts = nullptr;
    if (spec.single_image) {
        rc = av_dict_set(&raw_opts, "update", "1", 0);  // one image, no %d pattern
        if (rc < 0) {
            const ffmpeg::DictPtr cleanup(raw_opts);
            return detail::fail("av_dict_set", rc);
        }
    }
    rc = avformat_write_header(oc.get(), &raw_opts);
    const ffmpeg::DictPtr leftover_opts(raw_opts);
    if (rc < 0) {
        return detail::fail("avformat_write_header", rc);
    }

    ffmpeg::FramePtr frame(av_frame_alloc());
    ffmpeg::PacketPtr pkt(av_packet_alloc());
    if (!frame || !pkt) {
        return make_error(ErrorCode::EncodeError, "cannot allocate the frame or packet");
    }
    frame->format = spec.pix_fmt;
    frame->width = spec.width;
    frame->height = spec.height;
    rc = av_frame_get_buffer(frame.get(), 0);
    if (rc < 0) {
        return detail::fail("av_frame_get_buffer", rc);
    }
    ffmpeg::SwsPtr sws;
    if (spec.pix_fmt != AV_PIX_FMT_RGB24 && spec.pix_fmt != AV_PIX_FMT_RGBA) {
        sws.reset(sws_getContext(spec.width, spec.height, AV_PIX_FMT_RGB24, spec.width, spec.height,
                                 spec.pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws) {
            return make_error(ErrorCode::EncodeError, "sws_getContext failed");
        }
    }

    std::size_t packet_index = 0;
    // Sends `f` (null flushes) and writes every packet the encoder returns.
    const auto drain = [&](const AVFrame* f) -> Status {
        int r = avcodec_send_frame(enc.get(), f);
        if (r < 0) {
            return detail::fail("avcodec_send_frame", r);
        }
        for (;;) {
            r = avcodec_receive_packet(enc.get(), pkt.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                return Status::success();
            }
            if (r < 0) {
                return detail::fail("avcodec_receive_packet", r);
            }
            pkt->stream_index = st->index;
            if (packet_index < frames.size()) {
                pkt->duration = frames[packet_index].duration;
            }
            ++packet_index;
            av_packet_rescale_ts(pkt.get(), enc->time_base, st->time_base);
            r = av_interleaved_write_frame(oc.get(), pkt.get());
            if (r < 0) {
                return detail::fail("av_interleaved_write_frame", r);
            }
        }
    };

    const std::size_t bytes_per_pixel = spec.pix_fmt == AV_PIX_FMT_RGBA ? 4 : 3;
    const auto row_bytes = static_cast<std::size_t>(spec.width) * bytes_per_pixel;
    for (const FixtureFrame& f : frames) {
        rc = av_frame_make_writable(frame.get());
        if (rc < 0) {
            return detail::fail("av_frame_make_writable", rc);
        }
        if (sws) {
            const std::array<const std::uint8_t*, 4> src{f.rgb24.data(), nullptr, nullptr, nullptr};
            const std::array<int, 4> src_stride{spec.width * 3, 0, 0, 0};
            const int rows = sws_scale(sws.get(), src.data(), src_stride.data(), 0, spec.height,
                                       frame->data, frame->linesize);
            if (rows != spec.height) {
                return make_error(ErrorCode::EncodeError, "sws_scale failed");
            }
        } else {
            for (int y = 0; y < spec.height; ++y) {
                std::copy_n(f.rgb24.data() + static_cast<std::size_t>(y) * row_bytes, row_bytes,
                            frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0]);
            }
        }
        frame->pts = f.pts;
        frame->quality = enc->global_quality;
        if (Status s = drain(frame.get()); !s.ok()) {
            return s;
        }
    }
    if (Status s = drain(nullptr); !s.ok()) {
        return s;
    }

    rc = av_write_trailer(oc.get());
    if (rc < 0) {
        return detail::fail("av_write_trailer", rc);
    }
    if ((oc->oformat->flags & AVFMT_NOFILE) == 0) {
        rc = avio_closep(&oc->pb);
        if (rc < 0) {
            return detail::fail("avio_closep", rc);
        }
    }
    return Status::success();
}

[[nodiscard]] inline vglyph::Status write_png(const fs::path& path, int w, int h,
                                              std::vector<std::uint8_t> rgb24) {
    FixtureSpec spec;
    spec.codec_id = AV_CODEC_ID_PNG;
    spec.pix_fmt = AV_PIX_FMT_RGB24;
    spec.single_image = true;
    spec.width = w;
    spec.height = h;
    std::vector<FixtureFrame> frames(1);
    frames[0].rgb24 = std::move(rgb24);
    return write_ffmpeg_fixture(path, spec, frames);
}

// A PNG with an alpha channel; `rgba` is w * h * 4 bytes, straight (not premultiplied).
[[nodiscard]] inline vglyph::Status write_png_rgba(const fs::path& path, int w, int h,
                                                   std::vector<std::uint8_t> rgba) {
    FixtureSpec spec;
    spec.codec_id = AV_CODEC_ID_PNG;
    spec.pix_fmt = AV_PIX_FMT_RGBA;
    spec.single_image = true;
    spec.width = w;
    spec.height = h;
    std::vector<FixtureFrame> frames(1);
    frames[0].rgb24 = std::move(rgba);
    return write_ffmpeg_fixture(path, spec, frames);
}

// MPEG-4 in MP4, time base 1 ms. `frames[i].pts` / `.duration` are milliseconds.
[[nodiscard]] inline vglyph::Status write_mp4(const fs::path& path, int w, int h,
                                              const std::vector<FixtureFrame>& frames) {
    FixtureSpec spec;
    spec.codec_id = AV_CODEC_ID_MPEG4;
    spec.pix_fmt = AV_PIX_FMT_YUV420P;
    spec.width = w;
    spec.height = h;
    spec.time_base = AVRational{1, 1000};
    return write_ffmpeg_fixture(path, spec, frames);
}

// ---- Hand-written GIF --------------------------------------------------------

struct GifFrame {
    std::uint8_t color_index = 0;  // the whole frame is this palette entry
    std::uint16_t delay_cs = 0;    // hundredths of a second, written as is
    int transparent_index = -1;    // palette entry drawn as transparent; -1 for none
};

// A looping GIF89a with a 256-entry global palette whose first four entries are
// `palette`. LZW uses literal codes only, with a clear code often enough that
// the code width stays at 9 bits.
[[nodiscard]] inline std::vector<std::uint8_t> make_gif(std::uint16_t w, std::uint16_t h,
                                                        const std::array<vglyph::Rgb, 4>& palette,
                                                        const std::vector<GifFrame>& frames) {
    std::vector<std::uint8_t> g;
    const auto put = [&g](std::initializer_list<int> bytes) {
        for (const int b : bytes) {
            g.push_back(static_cast<std::uint8_t>(b));
        }
    };
    const auto put16 = [&put](unsigned v) { put({static_cast<int>(v & 0xFF), static_cast<int>(v >> 8)}); };

    for (const char c : std::string("GIF89a")) {
        put({c});
    }
    put16(w);
    put16(h);
    put({0xF7, 0, 0});  // global table, 256 entries; background 0; no aspect
    for (std::size_t i = 0; i < 256; ++i) {
        const vglyph::Rgb c = i < palette.size() ? palette[i] : vglyph::Rgb{0, 0, 0};
        put({c.r, c.g, c.b});
    }
    put({0x21, 0xFF, 0x0B});  // NETSCAPE2.0 looping extension, loop forever
    for (const char c : std::string("NETSCAPE2.0")) {
        put({c});
    }
    put({0x03, 0x01, 0x00, 0x00, 0x00});

    constexpr int kClear = 256;
    constexpr int kEnd = 257;
    constexpr std::size_t kCodesPerClear = 200;
    for (const GifFrame& f : frames) {
        const bool transparent = f.transparent_index >= 0;
        put({0x21, 0xF9, 0x04, transparent ? 0x05 : 0x04});  // graphic control: disposal 1
        put16(f.delay_cs);
        put({transparent ? f.transparent_index : 0, 0x00});
        put({0x2C});
        put16(0);
        put16(0);
        put16(w);
        put16(h);
        put({0x00, 8});  // no local table; LZW minimum code size 8

        std::vector<std::uint8_t> lzw;
        std::uint32_t acc = 0;
        int nbits = 0;
        const auto emit = [&](int code) {
            acc |= static_cast<std::uint32_t>(code) << nbits;
            nbits += 9;
            while (nbits >= 8) {
                lzw.push_back(static_cast<std::uint8_t>(acc & 0xFF));
                acc >>= 8;
                nbits -= 8;
            }
        };
        const std::size_t pixels = static_cast<std::size_t>(w) * h;
        for (std::size_t i = 0; i < pixels; ++i) {
            if (i % kCodesPerClear == 0) {
                emit(kClear);
            }
            emit(f.color_index);
        }
        emit(kEnd);
        if (nbits > 0) {
            lzw.push_back(static_cast<std::uint8_t>(acc & 0xFF));
        }
        for (std::size_t off = 0; off < lzw.size(); off += 255) {
            const std::size_t n = std::min<std::size_t>(255, lzw.size() - off);
            put({static_cast<int>(n)});
            g.insert(g.end(), lzw.begin() + static_cast<std::ptrdiff_t>(off),
                     lzw.begin() + static_cast<std::ptrdiff_t>(off + n));
        }
        put({0x00});
    }
    put({0x3B});
    return g;
}

}  // namespace vglyph_test
