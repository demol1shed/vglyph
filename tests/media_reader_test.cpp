#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include "media_fixture.hpp"
#include "test_framework.hpp"
#include "vglyph/convert.hpp"
#include "vglyph/media_reader.hpp"

using vglyph::ErrorCode;
using vglyph::MediaKind;
using vglyph::MediaReader;
using vglyph::ReadStatus;
using vglyph::Rgb;
using vglyph::VideoFrame;
using namespace vglyph_test;

namespace {

constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kWhite{255, 255, 255};

// A fixture that cannot be written is a failure; say why, since the cause is
// usually an FFmpeg build without the needed encoder.
bool fixture_ok(const vglyph::Status& status) {
    if (!status.ok()) {
        vglyph_test::out("  fixture failed: %s\n", status.error().detail.c_str());
    }
    return status.ok();
}

// Opens `path`, recording a failure (and leaving `reader` empty) if it cannot.
void open_ok(const fs::path& path, std::optional<MediaReader>& reader) {
    vglyph::Result<MediaReader> r = MediaReader::open(path);
    VGLYPH_CHECK(r.ok());
    if (MediaReader* opened = r.value_if()) {
        reader.emplace(std::move(*opened));
    } else if (const vglyph::Error* e = r.error_if()) {
        vglyph_test::out("  open failed: %s\n", e->detail.c_str());
    }
}

// Reads one frame; a decode error is recorded as a failure and reported as EndOfStream.
ReadStatus next(MediaReader& reader, std::uint32_t w, std::uint32_t h, VideoFrame& frame) {
    const vglyph::Result<ReadStatus> r = reader.read_frame(w, h, frame);
    const ReadStatus* status = r.value_if();
    VGLYPH_CHECK(status != nullptr);
    if (status == nullptr) {
        if (const vglyph::Error* e = r.error_if()) {
            vglyph_test::out("  read_frame failed: %s\n", e->detail.c_str());
        }
        return ReadStatus::EndOfStream;
    }
    return *status;
}

Rgb pixel(const VideoFrame& f, std::uint32_t x, std::uint32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * f.width + x) * 3;
    return Rgb{f.rgb24[i], f.rgb24[i + 1], f.rgb24[i + 2]};
}

bool near(Rgb p, Rgb want, int tolerance) {
    return std::abs(p.r - want.r) <= tolerance && std::abs(p.g - want.g) <= tolerance &&
           std::abs(p.b - want.b) <= tolerance;
}

bool has_code(const vglyph::Result<MediaReader>& r, ErrorCode code) {
    const vglyph::Error* e = r.error_if();
    return e != nullptr && e->code == code;
}

std::vector<FixtureFrame> mp4_frames(const std::vector<std::int64_t>& pts_ms,
                                     const std::vector<std::int64_t>& duration_ms,
                                     const std::vector<Rgb>& colors, int w, int h) {
    std::vector<FixtureFrame> frames(pts_ms.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i].rgb24 = solid_rgb(w, h, colors[i % colors.size()]);
        frames[i].pts = pts_ms[i];
        frames[i].duration = duration_ms[i];
    }
    return frames;
}

}  // namespace

// ---- Error paths --------------------------------------------------------------

VGLYPH_TEST(open_missing_file_is_io_error) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    VGLYPH_CHECK(has_code(MediaReader::open(dir.file("missing.png")), ErrorCode::IoError));
}

VGLYPH_TEST(open_garbage_is_unsupported_format) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const std::vector<std::uint8_t> junk(256, 'x');
    VGLYPH_CHECK(write_file(dir.file("junk.bin"), junk));
    VGLYPH_CHECK(has_code(MediaReader::open(dir.file("junk.bin")), ErrorCode::UnsupportedFormat));
}

VGLYPH_TEST(open_truncated_mp4_fails_cleanly) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const auto frames = mp4_frames({0, 100, 200}, {100, 100, 100}, {kRed, kGreen}, 32, 32);
    VGLYPH_CHECK(fixture_ok(write_mp4(dir.file("full.mp4"), 32, 32, frames)));
    std::vector<std::uint8_t> bytes;
    VGLYPH_CHECK(read_file(dir.file("full.mp4"), bytes));
    bytes.resize(bytes.size() / 2);  // the moov atom sits at the end of the file
    VGLYPH_CHECK(write_file(dir.file("cut.mp4"), bytes));
    VGLYPH_CHECK(!MediaReader::open(dir.file("cut.mp4")).ok());
}

VGLYPH_TEST(open_rejects_source_over_the_dimension_cap) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const int too_wide = static_cast<int>(vglyph::kMaxSourceDim) + 1;
    VGLYPH_CHECK(fixture_ok(write_png(dir.file("wide.png"), too_wide, 1,
                           solid_rgb(too_wide, 1, kWhite))));
    VGLYPH_CHECK(has_code(MediaReader::open(dir.file("wide.png")), ErrorCode::LimitExceeded));
    VGLYPH_CHECK(fixture_ok(write_png(dir.file("tall.png"), 1, too_wide,
                           solid_rgb(1, too_wide, kWhite))));
    VGLYPH_CHECK(has_code(MediaReader::open(dir.file("tall.png")), ErrorCode::LimitExceeded));
}

VGLYPH_TEST(read_frame_rejects_bad_target_sizes_without_consuming_a_frame) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    VGLYPH_CHECK(fixture_ok(write_png(dir.file("a.png"), 4, 4, solid_rgb(4, 4, kRed))));
    std::optional<MediaReader> reader;
    open_ok(dir.file("a.png"), reader);
    if (!reader) {
        return;
    }
    VideoFrame frame;
    const std::array<std::array<std::uint32_t, 2>, 3> bad{
        {{0, 2}, {2, 0}, {vglyph::kMaxSourceDim + 1, 2}}};
    for (const auto& size : bad) {
        const vglyph::Result<ReadStatus> r = reader->read_frame(size[0], size[1], frame);
        const vglyph::Error* e = r.error_if();
        VGLYPH_CHECK(e != nullptr && e->code == ErrorCode::InvalidArgument);
    }
    VGLYPH_CHECK(next(*reader, 2, 2, frame) == ReadStatus::Frame);  // still there
}

// ---- Still image --------------------------------------------------------------

VGLYPH_TEST(png_reads_scaled_quadrants_then_ends) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    VGLYPH_CHECK(fixture_ok(write_png(dir.file("q.png"), 4, 4,
                           quadrants_rgb(4, 4, {kRed, kGreen, kBlue, kWhite}))));
    std::optional<MediaReader> reader;
    open_ok(dir.file("q.png"), reader);
    if (!reader) {
        return;
    }
    const vglyph::MediaInfo& info = reader->info();
    VGLYPH_CHECK(info.media_kind == MediaKind::Image);
    VGLYPH_CHECK(info.src_width == 4 && info.src_height == 4);
    VGLYPH_CHECK(info.codec_name == "png");
    VGLYPH_CHECK(!info.pixel_format_name.empty());
    VGLYPH_CHECK(info.avg_fps == 0.0 && info.duration_us == 0);
    VGLYPH_CHECK(!info.has_audio && info.audio_stream_index == -1);

    VideoFrame frame;
    VGLYPH_CHECK(next(*reader, 2, 2, frame) == ReadStatus::Frame);
    VGLYPH_CHECK(frame.width == 2 && frame.height == 2 && frame.rgb24.size() == 12);
    VGLYPH_CHECK(frame.pts_us == 0 && frame.duration_us == 0);
    VGLYPH_CHECK(near(pixel(frame, 0, 0), kRed, 2));
    VGLYPH_CHECK(near(pixel(frame, 1, 0), kGreen, 2));
    VGLYPH_CHECK(near(pixel(frame, 0, 1), kBlue, 2));
    VGLYPH_CHECK(near(pixel(frame, 1, 1), kWhite, 2));

    VGLYPH_CHECK(next(*reader, 2, 2, frame) == ReadStatus::EndOfStream);
    VGLYPH_CHECK(next(*reader, 2, 2, frame) == ReadStatus::EndOfStream);  // and stays there
}

// ---- Animated GIF -------------------------------------------------------------

VGLYPH_TEST(gif_reads_frames_with_delays_and_the_zero_delay_clamp) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const std::vector<GifFrame> gif_frames{{0, 20}, {1, 0}, {2, 5}};  // red 200 ms, green 0 cs, blue 50 ms
    VGLYPH_CHECK(write_file(dir.file("a.gif"),
                            make_gif(4, 4, {kRed, kGreen, kBlue, kWhite}, gif_frames)));
    std::optional<MediaReader> reader;
    open_ok(dir.file("a.gif"), reader);
    if (!reader) {
        return;
    }
    VGLYPH_CHECK(reader->info().media_kind == MediaKind::AnimatedImage);
    VGLYPH_CHECK(reader->info().src_width == 4 && reader->info().src_height == 4);

    const std::array<Rgb, 3> colors{kRed, kGreen, kBlue};
    const std::array<std::int64_t, 3> pts_us{0, 200'000, 300'000};
    const std::array<std::int64_t, 3> duration_us{200'000, 100'000, 50'000};  // 0 cs becomes 100 ms
    VideoFrame frame;
    for (std::size_t i = 0; i < 3; ++i) {
        VGLYPH_CHECK(next(*reader, 4, 4, frame) == ReadStatus::Frame);
        VGLYPH_CHECK(near(pixel(frame, 1, 1), colors[i], 2));
        VGLYPH_CHECK(frame.pts_us == pts_us[i]);
        VGLYPH_CHECK(frame.duration_us == duration_us[i]);
    }
    VGLYPH_CHECK(next(*reader, 4, 4, frame) == ReadStatus::EndOfStream);
}

// ---- Video --------------------------------------------------------------------

VGLYPH_TEST(mp4_constant_frame_rate) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const auto frames = mp4_frames({0, 100, 200, 300, 400}, {100, 100, 100, 100, 100},
                                   {kRed, kGreen, kBlue, kWhite, kRed}, 32, 32);
    VGLYPH_CHECK(fixture_ok(write_mp4(dir.file("cfr.mp4"), 32, 32, frames)));
    std::optional<MediaReader> reader;
    open_ok(dir.file("cfr.mp4"), reader);
    if (!reader) {
        return;
    }
    const vglyph::MediaInfo& info = reader->info();
    VGLYPH_CHECK(info.media_kind == MediaKind::Video);
    VGLYPH_CHECK(info.src_width == 32 && info.src_height == 32);
    VGLYPH_CHECK(info.avg_fps > 9.5 && info.avg_fps < 10.5);
    VGLYPH_CHECK(info.duration_us > 0);

    const std::array<Rgb, 5> colors{kRed, kGreen, kBlue, kWhite, kRed};
    VideoFrame frame;
    for (std::int64_t i = 0; i < 5; ++i) {
        VGLYPH_CHECK(next(*reader, 8, 8, frame) == ReadStatus::Frame);
        VGLYPH_CHECK(frame.pts_us == i * 100'000);
        VGLYPH_CHECK(frame.duration_us == 100'000);
        VGLYPH_CHECK(near(pixel(frame, 4, 4), colors[static_cast<std::size_t>(i)], 40));
    }
    VGLYPH_CHECK(next(*reader, 8, 8, frame) == ReadStatus::EndOfStream);
}

VGLYPH_TEST(mp4_variable_frame_rate_keeps_per_frame_timing) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const auto frames = mp4_frames({0, 100, 150, 400}, {100, 50, 250, 100},
                                   {kRed, kGreen, kBlue, kWhite}, 32, 32);
    VGLYPH_CHECK(fixture_ok(write_mp4(dir.file("vfr.mp4"), 32, 32, frames)));
    std::optional<MediaReader> reader;
    open_ok(dir.file("vfr.mp4"), reader);
    if (!reader) {
        return;
    }
    const std::array<std::int64_t, 4> pts_us{0, 100'000, 150'000, 400'000};
    const std::array<std::int64_t, 4> duration_us{100'000, 50'000, 250'000, 100'000};
    VideoFrame frame;
    for (std::size_t i = 0; i < 4; ++i) {
        VGLYPH_CHECK(next(*reader, 8, 8, frame) == ReadStatus::Frame);
        VGLYPH_CHECK(frame.pts_us == pts_us[i]);
        VGLYPH_CHECK(frame.duration_us == duration_us[i]);
    }
    VGLYPH_CHECK(next(*reader, 8, 8, frame) == ReadStatus::EndOfStream);
}

VGLYPH_TEST(target_size_may_change_between_reads) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const auto frames = mp4_frames({0, 100, 200}, {100, 100, 100}, {kRed, kGreen, kBlue}, 32, 32);
    VGLYPH_CHECK(fixture_ok(write_mp4(dir.file("s.mp4"), 32, 32, frames)));
    std::optional<MediaReader> reader;
    open_ok(dir.file("s.mp4"), reader);
    if (!reader) {
        return;
    }
    VideoFrame frame;
    VGLYPH_CHECK(next(*reader, 8, 8, frame) == ReadStatus::Frame);
    VGLYPH_CHECK(frame.width == 8 && frame.height == 8 && frame.rgb24.size() == 8 * 8 * 3);
    VGLYPH_CHECK(near(pixel(frame, 4, 4), kRed, 40));
    VGLYPH_CHECK(next(*reader, 4, 2, frame) == ReadStatus::Frame);
    VGLYPH_CHECK(frame.width == 4 && frame.height == 2 && frame.rgb24.size() == 4 * 2 * 3);
    VGLYPH_CHECK(near(pixel(frame, 2, 1), kGreen, 40));
    VGLYPH_CHECK(next(*reader, 8, 8, frame) == ReadStatus::Frame);
    VGLYPH_CHECK(frame.width == 8 && frame.height == 8 && frame.rgb24.size() == 8 * 8 * 3);
    VGLYPH_CHECK(near(pixel(frame, 4, 4), kBlue, 40));
}

VGLYPH_TEST_MAIN()
