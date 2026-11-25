#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

#include "media_fixture.hpp"
#include "quantize.hpp"
#include "test_framework.hpp"
#include "vglyph/convert.hpp"
#include "vglyph/media_exporter.hpp"
#include "vglyph/media_reader.hpp"

using vglyph::ErrorCode;
using vglyph::ExportFormat;
using vglyph::ExportOptions;
using vglyph::MediaExporter;
using vglyph::MediaReader;
using vglyph::ReadStatus;
using vglyph::RgbaImage;
using vglyph::Rgb;
using vglyph::VideoFrame;
using namespace vglyph_test;

#if defined(__SANITIZE_ADDRESS__)
#define VGLYPH_TEST_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VGLYPH_TEST_HAS_ASAN 1
#endif
#endif

#ifdef VGLYPH_TEST_HAS_ASAN
// ASan runtime hook, which also sees FFmpeg's av_malloc. Declared here because
// GCC does not ship the header for it.
extern "C" int __sanitizer_install_malloc_and_free_hooks(
    void (*malloc_hook)(const volatile void*, std::size_t),
    void (*free_hook)(const volatile void*));
#endif

namespace {

constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kReadBackground{9, 9, 9};  // never visible: exported frames are opaque

RgbaImage solid(std::uint32_t w, std::uint32_t h, Rgb c) {
    RgbaImage image;
    image.width = w;
    image.height = h;
    image.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = c.r;
        image.rgba[i + 1] = c.g;
        image.rgba[i + 2] = c.b;
        image.rgba[i + 3] = 255;
    }
    return image;
}

bool has_code(const vglyph::Status& s, ErrorCode code) {
    return !s.ok() && s.error().code == code;
}

// Opens an exporter, recording a failure (and leaving `out` empty) if it cannot.
void open_exporter(const fs::path& path, ExportFormat format, std::uint32_t w, std::uint32_t h,
                   std::optional<MediaExporter>& out, const ExportOptions& options = {}) {
    vglyph::Result<MediaExporter> r = MediaExporter::open(path, format, w, h, options);
    VGLYPH_CHECK(r.ok());
    if (MediaExporter* opened = r.value_if()) {
        out.emplace(std::move(*opened));
    } else if (const vglyph::Error* e = r.error_if()) {
        vglyph_test::out("  open failed: %s\n", e->detail.c_str());
    }
}

void open_reader(const fs::path& path, std::optional<MediaReader>& out) {
    vglyph::Result<MediaReader> r = MediaReader::open(path);
    VGLYPH_CHECK(r.ok());
    if (MediaReader* opened = r.value_if()) {
        out.emplace(std::move(*opened));
    } else if (const vglyph::Error* e = r.error_if()) {
        vglyph_test::out("  reopen failed: %s\n", e->detail.c_str());
    }
}

// Reads one frame; a decode error is a recorded failure reported as EndOfStream.
ReadStatus next(MediaReader& reader, std::uint32_t w, std::uint32_t h, VideoFrame& frame) {
    const vglyph::Result<ReadStatus> r = reader.read_frame(w, h, frame, kReadBackground);
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

}  // namespace

// ---- PNG ----------------------------------------------------------------------

VGLYPH_TEST(png_round_trips_every_pixel_exactly) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    RgbaImage image = solid(5, 3, kBlue);
    for (std::uint32_t y = 0; y < 3; ++y) {
        for (std::uint32_t x = 0; x < 5; ++x) {
            std::uint8_t* px = &image.rgba[(static_cast<std::size_t>(y) * 5 + x) * 4];
            px[0] = static_cast<std::uint8_t>(x * 50);
            px[1] = static_cast<std::uint8_t>(y * 100);
            px[2] = static_cast<std::uint8_t>(x * y * 10);
        }
    }
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.png"), ExportFormat::Png, 5, 3, exporter);
    if (!exporter) {
        return;
    }
    VGLYPH_CHECK(exporter->write_frame(image, 0, 0).ok());
    VGLYPH_CHECK(exporter->finish().ok());

    std::optional<MediaReader> reader;
    open_reader(dir.file("a.png"), reader);
    if (!reader) {
        return;
    }
    VGLYPH_CHECK(reader->info().codec_name == "png");
    VGLYPH_CHECK(reader->info().src_width == 5 && reader->info().src_height == 3);
    VideoFrame frame;
    VGLYPH_CHECK(next(*reader, 5, 3, frame) == ReadStatus::Frame);
    for (std::uint32_t y = 0; y < 3; ++y) {
        for (std::uint32_t x = 0; x < 5; ++x) {
            const std::uint8_t* px = &image.rgba[(static_cast<std::size_t>(y) * 5 + x) * 4];
            VGLYPH_CHECK(near(pixel(frame, x, y), Rgb{px[0], px[1], px[2]}, 0));
        }
    }
}

VGLYPH_TEST(png_takes_exactly_one_frame) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.png"), ExportFormat::Png, 4, 4, exporter);
    if (!exporter) {
        return;
    }
    const RgbaImage image = solid(4, 4, kRed);
    VGLYPH_CHECK(exporter->write_frame(image, 0, 0).ok());
    VGLYPH_CHECK(has_code(exporter->write_frame(image, 0, 0), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(exporter->finish().ok());  // the rejected frame did not spoil the file

    std::optional<MediaReader> reader;
    open_reader(dir.file("a.png"), reader);
    if (!reader) {
        return;
    }
    VideoFrame frame;
    VGLYPH_CHECK(next(*reader, 4, 4, frame) == ReadStatus::Frame);
    VGLYPH_CHECK(near(pixel(frame, 1, 1), kRed, 0));
}

// ---- GIF ----------------------------------------------------------------------

VGLYPH_TEST(gif_keeps_frame_times_and_colors) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.gif"), ExportFormat::Gif, 16, 16, exporter);
    if (!exporter) {
        return;
    }
    const std::array<Rgb, 3> colors{kRed, kGreen, kBlue};
    const std::array<std::int64_t, 3> pts_us{0, 30'000, 100'000};
    const std::array<std::int64_t, 3> duration_us{30'000, 70'000, 100'000};
    for (std::size_t i = 0; i < 3; ++i) {
        VGLYPH_CHECK(exporter->write_frame(solid(16, 16, colors[i]), pts_us[i], duration_us[i]).ok());
    }
    VGLYPH_CHECK(exporter->finish().ok());

    std::optional<MediaReader> reader;
    open_reader(dir.file("a.gif"), reader);
    if (!reader) {
        return;
    }
    VGLYPH_CHECK(reader->info().media_kind == vglyph::MediaKind::AnimatedImage);
    VideoFrame frame;
    for (std::size_t i = 0; i < 3; ++i) {
        VGLYPH_CHECK(next(*reader, 16, 16, frame) == ReadStatus::Frame);
        VGLYPH_CHECK(near(pixel(frame, 8, 8), colors[i], 2));
        VGLYPH_CHECK(frame.pts_us == pts_us[i]);
        VGLYPH_CHECK(frame.duration_us == duration_us[i]);
    }
    VGLYPH_CHECK(next(*reader, 16, 16, frame) == ReadStatus::EndOfStream);
}

VGLYPH_TEST(gif_rounds_cumulative_times_without_drift) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.gif"), ExportFormat::Gif, 4, 4, exporter);
    if (!exporter) {
        return;
    }
    // 33.333 ms frames: each rounded to 3 cs would lose 1 cs every 3 frames.
    // The cumulative times 0, 33.3, 66.7, 100 ms round to 0, 3, 7 and 10 cs.
    const std::array<Rgb, 4> colors{kRed, kGreen, kBlue, kWhite};
    for (std::int64_t i = 0; i < 4; ++i) {
        VGLYPH_CHECK(exporter
                         ->write_frame(solid(4, 4, colors[static_cast<std::size_t>(i)]),
                                       i * 33'333, 33'333)
                         .ok());
    }
    VGLYPH_CHECK(exporter->finish().ok());

    std::optional<MediaReader> reader;
    open_reader(dir.file("a.gif"), reader);
    if (!reader) {
        return;
    }
    const std::array<std::int64_t, 4> pts_us{0, 30'000, 70'000, 100'000};
    VideoFrame frame;
    for (std::size_t i = 0; i < 4; ++i) {
        VGLYPH_CHECK(next(*reader, 4, 4, frame) == ReadStatus::Frame);
        VGLYPH_CHECK(frame.pts_us == pts_us[i]);
    }
}

VGLYPH_TEST(gif_frames_shorter_than_one_centisecond_still_advance) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.gif"), ExportFormat::Gif, 4, 4, exporter);
    if (!exporter) {
        return;
    }
    // Three frames inside one centisecond would share a time, and 0 cs means "default".
    VGLYPH_CHECK(exporter->write_frame(solid(4, 4, kRed), 0, 1'000).ok());
    VGLYPH_CHECK(exporter->write_frame(solid(4, 4, kGreen), 1'000, 1'000).ok());
    VGLYPH_CHECK(exporter->write_frame(solid(4, 4, kBlue), 2'000, 1'000).ok());
    VGLYPH_CHECK(exporter->finish().ok());

    std::optional<MediaReader> reader;
    open_reader(dir.file("a.gif"), reader);
    if (!reader) {
        return;
    }
    VideoFrame frame;
    std::size_t frames = 0;
    while (next(*reader, 4, 4, frame) == ReadStatus::Frame) {
        ++frames;
    }
    VGLYPH_CHECK(frames == 3);
}

// The encoder stores only what changed since the frame it still holds, so reusing
// that frame's buffer would corrupt the next one.
VGLYPH_TEST(gif_frames_with_partial_changes_round_trip) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    constexpr std::uint32_t kSize = 32;
    constexpr std::uint32_t kSquare = 6;
    constexpr std::uint32_t kFrames = 6;
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.gif"), ExportFormat::Gif, kSize, kSize, exporter);
    if (!exporter) {
        return;
    }
    // A red square moves diagonally over a blue background. It never covers the
    // first pixel, so every frame gets the same palette order and the encoder
    // compares frames with each other.
    const auto inside = [](std::uint32_t i, std::uint32_t x, std::uint32_t y) {
        const std::uint32_t start = 2 + i * 4;
        return x >= start && x < start + kSquare && y >= start && y < start + kSquare;
    };
    RgbaImage image = solid(kSize, kSize, kBlue);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const Rgb c = inside(i, x, y) ? kRed : kBlue;
                std::uint8_t* px = &image.rgba[(static_cast<std::size_t>(y) * kSize + x) * 4];
                px[0] = c.r;
                px[1] = c.g;
                px[2] = c.b;
            }
        }
        VGLYPH_CHECK(exporter->write_frame(image, std::int64_t{i} * 100'000, 100'000).ok());
    }
    VGLYPH_CHECK(exporter->finish().ok());

    std::optional<MediaReader> reader;
    open_reader(dir.file("a.gif"), reader);
    if (!reader) {
        return;
    }
    VideoFrame frame;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        VGLYPH_CHECK(next(*reader, kSize, kSize, frame) == ReadStatus::Frame);
        bool matches = true;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                matches = matches && near(pixel(frame, x, y), inside(i, x, y) ? kRed : kBlue, 2);
            }
        }
        VGLYPH_CHECK(matches);
    }
    VGLYPH_CHECK(next(*reader, kSize, kSize, frame) == ReadStatus::EndOfStream);
}

#ifdef VGLYPH_TEST_HAS_ASAN
namespace {

constexpr std::uint32_t kAllocSize = 512;
// A 512x512 palette frame is one buffer slightly over width * height bytes (it
// also holds the palette). Exactly width * height is excluded: the encoder and
// the quantizer allocate that once. The encoder's packet buffer is far larger.
constexpr std::size_t kPixelBytes = std::size_t{kAllocSize} * kAllocSize;
constexpr std::size_t kFrameBytesMax = kPixelBytes + 16384;

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_frame_allocations{0};

void count_frame_allocation(const volatile void* /*ptr*/, std::size_t size) {
    if (g_counting.load() && size > kPixelBytes && size < kFrameBytesMax) {
        g_frame_allocations.fetch_add(1);
    }
}
void ignore_free(const volatile void* /*ptr*/) {}

}  // namespace

VGLYPH_TEST(gif_export_does_not_allocate_a_frame_per_write) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    VGLYPH_CHECK(__sanitizer_install_malloc_and_free_hooks(count_frame_allocation, ignore_free) !=
                 0);
    const RgbaImage red = solid(kAllocSize, kAllocSize, kRed);
    const RgbaImage blue = solid(kAllocSize, kAllocSize, kBlue);

    g_frame_allocations.store(0);
    g_counting.store(true);
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.gif"), ExportFormat::Gif, kAllocSize, kAllocSize, exporter);
    g_counting.store(false);
    VGLYPH_CHECK(g_frame_allocations.load() > 0);  // the size window does see frame buffers
    if (!exporter) {
        return;
    }
    g_frame_allocations.store(0);
    for (std::int64_t i = 0; i < 8; ++i) {
        g_counting.store(true);
        const vglyph::Status written =
            exporter->write_frame(i % 2 == 0 ? red : blue, i * 100'000, 100'000);
        g_counting.store(false);
        VGLYPH_CHECK(written.ok());
    }
    VGLYPH_CHECK(g_frame_allocations.load() == 0);
    VGLYPH_CHECK(exporter->finish().ok());
}
#endif

// ---- MP4 ----------------------------------------------------------------------

VGLYPH_TEST(mp4_pads_odd_sizes_and_keeps_variable_frame_times) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    ExportOptions options;
    options.bg_color = Rgb{40, 40, 40};  // neutral grey: no chroma shift next to white
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.mp4"), ExportFormat::Mp4, 33, 17, exporter, options);
    if (!exporter) {
        return;
    }
    const std::array<Rgb, 4> colors{kWhite, kGreen, kBlue, kRed};
    const std::array<std::int64_t, 4> pts_us{0, 100'000, 150'000, 400'000};
    const std::array<std::int64_t, 4> duration_us{100'000, 50'000, 250'000, 100'000};
    for (std::size_t i = 0; i < 4; ++i) {
        VGLYPH_CHECK(exporter->write_frame(solid(33, 17, colors[i]), pts_us[i], duration_us[i]).ok());
    }
    VGLYPH_CHECK(exporter->finish().ok());

    std::optional<MediaReader> reader;
    open_reader(dir.file("a.mp4"), reader);
    if (!reader) {
        return;
    }
    VGLYPH_CHECK(reader->info().codec_name == "h264");
    VGLYPH_CHECK(reader->info().src_width == 34 && reader->info().src_height == 18);

    VideoFrame frame;
    for (std::size_t i = 0; i < 4; ++i) {
        VGLYPH_CHECK(next(*reader, 34, 18, frame) == ReadStatus::Frame);
        VGLYPH_CHECK(frame.pts_us == pts_us[i]);
        VGLYPH_CHECK(frame.duration_us == duration_us[i]);
        VGLYPH_CHECK(near(pixel(frame, 15, 8), colors[i], 40));
        if (i == 0) {
            VGLYPH_CHECK(near(pixel(frame, 33, 17), options.bg_color, 12));  // the padding corner
            VGLYPH_CHECK(near(pixel(frame, 33, 4), options.bg_color, 12));   // the padding column
            VGLYPH_CHECK(near(pixel(frame, 4, 17), options.bg_color, 12));   // the padding row
        }
    }
    VGLYPH_CHECK(next(*reader, 34, 18, frame) == ReadStatus::EndOfStream);
}

VGLYPH_TEST(mp4_even_sizes_are_not_padded) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.mp4"), ExportFormat::Mp4, 32, 16, exporter);
    if (!exporter) {
        return;
    }
    VGLYPH_CHECK(exporter->write_frame(solid(32, 16, kWhite), 0, 100'000).ok());
    VGLYPH_CHECK(exporter->finish().ok());
    std::optional<MediaReader> reader;
    open_reader(dir.file("a.mp4"), reader);
    if (!reader) {
        return;
    }
    VGLYPH_CHECK(reader->info().src_width == 32 && reader->info().src_height == 16);
}

VGLYPH_TEST(mp4_accepts_the_whole_crf_range_and_rejects_the_rest) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    for (const int crf : {0, 20, 51}) {
        ExportOptions options;
        options.crf = crf;
        std::optional<MediaExporter> exporter;
        open_exporter(dir.file("a.mp4"), ExportFormat::Mp4, 16, 16, exporter, options);
        if (!exporter) {
            return;
        }
        VGLYPH_CHECK(exporter->write_frame(solid(16, 16, kRed), 0, 100'000).ok());
        VGLYPH_CHECK(exporter->finish().ok());
    }
    for (const int crf : {-1, 52}) {
        ExportOptions options;
        options.crf = crf;
        const vglyph::Result<MediaExporter> r =
            MediaExporter::open(dir.file("b.mp4"), ExportFormat::Mp4, 16, 16, options);
        const vglyph::Error* e = r.error_if();
        VGLYPH_CHECK(e != nullptr && e->code == ErrorCode::InvalidArgument);
    }
}

VGLYPH_TEST(mp4_rejects_frame_times_that_do_not_increase) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.mp4"), ExportFormat::Mp4, 16, 16, exporter);
    if (!exporter) {
        return;
    }
    const RgbaImage image = solid(16, 16, kRed);
    VGLYPH_CHECK(exporter->write_frame(image, 100'000, 100'000).ok());
    VGLYPH_CHECK(has_code(exporter->write_frame(image, 100'000, 100'000), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(exporter->write_frame(image, 50'000, 100'000), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(exporter->write_frame(image, 200'000, 100'000).ok());  // still usable
    VGLYPH_CHECK(exporter->finish().ok());
}

// ---- Error paths --------------------------------------------------------------

VGLYPH_TEST(open_rejects_bad_sizes) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const std::array<ExportFormat, 3> formats{ExportFormat::Png, ExportFormat::Gif, ExportFormat::Mp4};
    for (const ExportFormat format : formats) {
        const vglyph::Result<MediaExporter> zero = MediaExporter::open(dir.file("z"), format, 0, 4);
        VGLYPH_CHECK(zero.error_if() != nullptr &&
                     zero.error_if()->code == ErrorCode::InvalidArgument);
        const vglyph::Result<MediaExporter> wide =
            MediaExporter::open(dir.file("w"), format, vglyph::kMaxSourceDim + 1, 1);
        VGLYPH_CHECK(wide.error_if() != nullptr &&
                     wide.error_if()->code == ErrorCode::LimitExceeded);
        // 16384 x 16384 is 256 MP: under the per-axis cap, over the pixel cap.
        const vglyph::Result<MediaExporter> huge =
            MediaExporter::open(dir.file("h"), format, vglyph::kMaxSourceDim, vglyph::kMaxSourceDim);
        VGLYPH_CHECK(huge.error_if() != nullptr &&
                     huge.error_if()->code == ErrorCode::LimitExceeded);
    }
    std::error_code ec;
    VGLYPH_CHECK(!fs::exists(dir.file("z"), ec) && !fs::exists(dir.file("w"), ec) &&
                 !fs::exists(dir.file("h"), ec));  // nothing was created before the checks
}

VGLYPH_TEST(open_in_a_missing_directory_is_io_error) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const std::array<ExportFormat, 3> formats{ExportFormat::Png, ExportFormat::Gif, ExportFormat::Mp4};
    for (const ExportFormat format : formats) {
        const vglyph::Result<MediaExporter> r =
            MediaExporter::open(dir.file("no-such-dir") / "a.out", format, 4, 4);
        VGLYPH_CHECK(r.error_if() != nullptr && r.error_if()->code == ErrorCode::IoError);
    }
}

VGLYPH_TEST(write_frame_checks_its_arguments) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("a.gif"), ExportFormat::Gif, 4, 4, exporter);
    if (!exporter) {
        return;
    }
    VGLYPH_CHECK(has_code(exporter->write_frame(solid(5, 4, kRed), 0, 10'000), ErrorCode::InvalidArgument));
    RgbaImage short_buffer = solid(4, 4, kRed);
    short_buffer.rgba.pop_back();
    VGLYPH_CHECK(has_code(exporter->write_frame(short_buffer, 0, 10'000), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(exporter->write_frame(solid(4, 4, kRed), -1, 10'000), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(exporter->write_frame(solid(4, 4, kRed), 0, -1), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(exporter->write_frame(solid(4, 4, kRed), INT64_MAX, 1), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(exporter->write_frame(solid(4, 4, kRed), 0, 10'000).ok());  // none of that spoiled it
    VGLYPH_CHECK(exporter->finish().ok());
}

VGLYPH_TEST(finish_needs_a_frame_and_closes_the_exporter) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    std::optional<MediaExporter> empty;
    open_exporter(dir.file("a.png"), ExportFormat::Png, 4, 4, empty);
    if (!empty) {
        return;
    }
    VGLYPH_CHECK(has_code(empty->finish(), ErrorCode::InvalidArgument));

    std::optional<MediaExporter> exporter;
    open_exporter(dir.file("b.gif"), ExportFormat::Gif, 4, 4, exporter);
    if (!exporter) {
        return;
    }
    VGLYPH_CHECK(exporter->write_frame(solid(4, 4, kRed), 0, 10'000).ok());
    VGLYPH_CHECK(exporter->finish().ok());
    VGLYPH_CHECK(has_code(exporter->finish(), ErrorCode::InvalidArgument));
    VGLYPH_CHECK(has_code(exporter->write_frame(solid(4, 4, kRed), 20'000, 10'000),
                          ErrorCode::InvalidArgument));
}

// ---- Quantizer ----------------------------------------------------------------

namespace {

std::uint32_t pack(Rgb c) {
    return 0xFF000000u | (static_cast<std::uint32_t>(c.r) << 16) |
           (static_cast<std::uint32_t>(c.g) << 8) | c.b;
}

Rgb unpack(std::uint32_t argb) {
    return Rgb{static_cast<std::uint8_t>(argb >> 16), static_cast<std::uint8_t>(argb >> 8),
               static_cast<std::uint8_t>(argb)};
}

// `distinct` colors laid out row by row, repeated to fill `pixels`.
std::vector<std::uint8_t> many_colors(std::size_t pixels, std::size_t distinct) {
    std::vector<std::uint8_t> rgba(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t k = i % distinct;
        rgba[i * 4] = static_cast<std::uint8_t>(k * 255 / (distinct > 1 ? distinct - 1 : 1));
        rgba[i * 4 + 1] = static_cast<std::uint8_t>((k * 37) % 256);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>((k * 91) % 256);
        rgba[i * 4 + 3] = 255;
    }
    return rgba;
}

}  // namespace

VGLYPH_TEST(quantizer_keeps_up_to_256_colors_exactly) {
    constexpr std::size_t kPixels = 4000;
    // Distinct in the low bits too, so a 5-bit histogram alone would merge them.
    std::vector<std::uint8_t> rgba(kPixels * 4);
    for (std::size_t i = 0; i < kPixels; ++i) {
        const std::size_t k = i % 256;
        rgba[i * 4] = static_cast<std::uint8_t>(k);
        rgba[i * 4 + 1] = static_cast<std::uint8_t>(255 - k);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(k ^ 0x55);
        rgba[i * 4 + 3] = 255;
    }
    vglyph::Quantizer quantizer;
    std::vector<std::uint8_t> indices(kPixels);
    std::array<std::uint32_t, vglyph::kPaletteSize> palette{};
    quantizer.quantize(rgba.data(), kPixels, indices.data(), palette.data());
    bool exact = true;
    for (std::size_t i = 0; i < kPixels; ++i) {
        exact = exact && palette[indices[i]] == pack(Rgb{rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]});
    }
    VGLYPH_CHECK(exact);
}

VGLYPH_TEST(quantizer_approximates_more_than_256_colors) {
    constexpr std::size_t kPixels = 64 * 64;
    // A smooth gradient with far more than 256 distinct colors.
    std::vector<std::uint8_t> rgba(kPixels * 4);
    for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            std::uint8_t* px = &rgba[(y * 64 + x) * 4];
            px[0] = static_cast<std::uint8_t>(x * 4);
            px[1] = static_cast<std::uint8_t>(y * 4);
            px[2] = static_cast<std::uint8_t>((x + y) * 2);
            px[3] = 255;
        }
    }
    vglyph::Quantizer quantizer;
    std::vector<std::uint8_t> indices(kPixels);
    std::array<std::uint32_t, vglyph::kPaletteSize> palette{};
    quantizer.quantize(rgba.data(), kPixels, indices.data(), palette.data());

    int worst = 0;
    for (std::size_t i = 0; i < kPixels; ++i) {
        const Rgb got = unpack(palette[indices[i]]);
        worst = std::max({worst, std::abs(got.r - rgba[i * 4]), std::abs(got.g - rgba[i * 4 + 1]),
                          std::abs(got.b - rgba[i * 4 + 2])});
    }
    if (worst > 24) {
        vglyph_test::out("  worst channel error %d\n", worst);
    }
    VGLYPH_CHECK(worst <= 24);
}

VGLYPH_TEST(quantizer_is_deterministic_and_reusable) {
    constexpr std::size_t kPixels = 3000;
    const std::vector<std::uint8_t> busy = many_colors(kPixels, 1000);
    const std::vector<std::uint8_t> other = many_colors(kPixels, 700);

    vglyph::Quantizer fresh;
    std::vector<std::uint8_t> want_indices(kPixels);
    std::array<std::uint32_t, vglyph::kPaletteSize> want_palette{};
    fresh.quantize(busy.data(), kPixels, want_indices.data(), want_palette.data());

    // The same instance after a different image must give the same answer as a fresh one.
    vglyph::Quantizer reused;
    std::vector<std::uint8_t> indices(kPixels);
    std::array<std::uint32_t, vglyph::kPaletteSize> palette{};
    reused.quantize(other.data(), kPixels, indices.data(), palette.data());
    reused.quantize(busy.data(), kPixels, indices.data(), palette.data());
    VGLYPH_CHECK(indices == want_indices);
    VGLYPH_CHECK(palette == want_palette);
}

VGLYPH_TEST_MAIN()
