#pragma once

// Internal helper (not installed): move-only owners for the FFmpeg C handles.
// Every handle in vglyph lives in one of these, never in a raw pointer.

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace vglyph::ffmpeg {

struct FormatCtxDeleter {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
};
struct CodecCtxDeleter {
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
};
struct FrameDeleter {
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* p) const noexcept { sws_freeContext(p); }
};
struct DictDeleter {
    void operator()(AVDictionary* p) const noexcept { av_dict_free(&p); }
};

using FormatCtx = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using CodecCtx = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using DictPtr = std::unique_ptr<AVDictionary, DictDeleter>;

// Releases the buffers a decoded frame holds when the scope ends, so every
// return path hands the frame back to the decoder.
class FrameUnref {
public:
    explicit FrameUnref(AVFrame* frame) noexcept : frame_(frame) {}
    ~FrameUnref() { av_frame_unref(frame_); }
    FrameUnref(const FrameUnref&) = delete;
    FrameUnref& operator=(const FrameUnref&) = delete;

private:
    AVFrame* frame_;
};

}  // namespace vglyph::ffmpeg
