#include <algorithm>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "commands.hpp"
#include "media_fixture.hpp"
#include "platform.hpp"
#include "test_framework.hpp"
#include "vglyph/rasterizer.hpp"

using vglyph::ErrorCode;
using vglyph::Rgb;
using namespace vglyph::cli;
using namespace vglyph_test;

namespace {

constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kWhite{255, 255, 255};

std::string utf8(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

int run_cli(const std::vector<std::string>& args) {
    const std::vector<std::string_view> views(args.begin(), args.end());
    return run(views);
}

std::string read_text(const fs::path& path) {
    std::vector<std::uint8_t> bytes;
    VGLYPH_CHECK(read_file(path, bytes));
    return std::string(bytes.begin(), bytes.end());
}

// An 8x8 all-white PNG in `dir`; with -w 4 it becomes a 4x2 grid.
fs::path white_png(const TempDir& dir) {
    const fs::path path = dir.file("white.png");
    VGLYPH_CHECK(write_png(path, 8, 8, solid_rgb(8, 8, kWhite)).ok());
    return path;
}

// A 4x4 GIF with three frames: black, white, black.
fs::path bw_gif(const TempDir& dir) {
    const fs::path path = dir.file("bw.gif");
    const std::vector<std::uint8_t> bytes =
        make_gif(4, 4, {kBlack, kWhite, kBlack, kBlack}, {{0, 10}, {1, 10}, {0, 10}});
    VGLYPH_CHECK(write_file(path, bytes));
    return path;
}

// Every frame of a media file, decoded at its own size.
struct Decoded {
    vglyph::MediaInfo info;
    std::vector<vglyph::VideoFrame> frames;
};

Decoded decode(const fs::path& path) {
    Decoded out;
    vglyph::Result<vglyph::MediaReader> opened = vglyph::MediaReader::open(path);
    vglyph::MediaReader* reader = opened.value_if();
    VGLYPH_CHECK(reader != nullptr);
    if (reader == nullptr) {
        return out;
    }
    out.info = reader->info();
    for (;;) {
        vglyph::VideoFrame frame;
        const auto read = reader->read_frame(out.info.src_width, out.info.src_height, frame, kBlack);
        const vglyph::ReadStatus* status = read.value_if();
        VGLYPH_CHECK(status != nullptr);
        if (status == nullptr || *status == vglyph::ReadStatus::EndOfStream) {
            return out;
        }
        out.frames.push_back(std::move(frame));
    }
}

bool has_bright_pixel(const vglyph::VideoFrame& frame) {
    return std::ranges::any_of(frame.rgb24, [](std::uint8_t v) { return v > 200; });
}

// The default 16 px font has 10x22 cells, so an 8x8 source at -w 4 is a 4x2 grid.
constexpr std::uint32_t kCellW = 10;
constexpr std::uint32_t kCellH = 22;

bool is_size(const Decoded& d, std::uint32_t w, std::uint32_t h) {
    return d.info.src_width == w && d.info.src_height == h;
}

// No temporary export file is left beside `out`.
bool no_temp(fs::path out) {
    out += ".vglyph-tmp";
    std::error_code ec;
    return !fs::exists(out, ec);
}

}  // namespace

VGLYPH_TEST(convert_png_to_png_uses_the_font_cell_shape) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = white_png(dir);
    const fs::path out = dir.file("out.png");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono"}) == 0);
    Decoded d = decode(out);
    VGLYPH_CHECK(d.info.codec_name == "png" && is_size(d, 4 * kCellW, 2 * kCellH));
    VGLYPH_CHECK(d.frames.size() == 1 && has_bright_pixel(d.frames.front()));
    VGLYPH_CHECK(no_temp(out));

    // An explicit --aspect wins over the font's cell shape: 4 rows. The old file is replaced.
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--aspect", "1"}) == 0);
    VGLYPH_CHECK(is_size(decode(out), 4 * kCellW, 4 * kCellH));

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--font-size", "32"}) == 0);
    d = decode(out);
    VGLYPH_CHECK(d.info.src_width > 4 * kCellW && d.info.src_height > 2 * kCellH);
}

VGLYPH_TEST(convert_png_output_takes_the_requested_frame) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = bw_gif(dir);
    const fs::path out = dir.file("out.png");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono"}) == 0);
    Decoded d = decode(out);
    VGLYPH_CHECK(d.frames.size() == 1 && !has_bright_pixel(d.frames.front()));

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono", "--frame", "1"}) == 0);
    d = decode(out);
    VGLYPH_CHECK(d.frames.size() == 1 && has_bright_pixel(d.frames.front()));

    const fs::path past = dir.file("past.png");
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(past), "--frame", "3"}) == 2);
    VGLYPH_CHECK(!fs::exists(past) && no_temp(past));
}

VGLYPH_TEST(convert_gif_to_gif_and_mp4_keeps_every_frame_and_time) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = bw_gif(dir);  // three frames of 10 cs, read as 100 ms each

    for (const char* name : {"out.gif", "out.mp4"}) {
        const fs::path out = dir.file(name);
        VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono"}) == 0);
        const Decoded d = decode(out);
        VGLYPH_CHECK(is_size(d, 4 * kCellW, 2 * kCellH));
        VGLYPH_CHECK(d.frames.size() == 3);
        if (d.frames.size() == 3) {
            for (std::size_t i = 0; i < 3; ++i) {
                VGLYPH_CHECK(d.frames[i].pts_us == static_cast<std::int64_t>(i) * 100'000);
                VGLYPH_CHECK(has_bright_pixel(d.frames[i]) == (i == 1));
            }
        }
        VGLYPH_CHECK(no_temp(out));
    }
}

VGLYPH_TEST(convert_video_to_mp4_keeps_variable_frame_times) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = dir.file("in.mp4");
    std::vector<FixtureFrame> frames(3);
    const std::int64_t pts_ms[3] = {0, 40, 300};
    for (std::size_t i = 0; i < 3; ++i) {
        frames[i] = FixtureFrame{solid_rgb(32, 32, kWhite), pts_ms[i], 40};
    }
    VGLYPH_CHECK(write_mp4(in, 32, 32, frames).ok());
    const fs::path out = dir.file("out.mp4");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--crf", "0"}) == 0);
    const Decoded d = decode(out);
    VGLYPH_CHECK(d.info.codec_name == "h264" && d.frames.size() == 3);
    if (d.frames.size() == 3) {
        for (std::size_t i = 0; i < 3; ++i) {
            VGLYPH_CHECK(d.frames[i].pts_us == pts_ms[i] * 1000);
        }
    }
}

VGLYPH_TEST(a_photo_cannot_become_an_animation) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = white_png(dir);
    for (const char* name : {"out.gif", "out.mp4"}) {
        const fs::path out = dir.file(name);
        VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out)}) == 2);
        VGLYPH_CHECK(!fs::exists(out) && no_temp(out));
    }
}

VGLYPH_TEST(custom_font_is_loaded_and_bad_fonts_fail) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = white_png(dir);
    const fs::path font = dir.file("copy.ttf");
    const auto data = vglyph::default_font_data();
    VGLYPH_CHECK(write_file(font, std::vector<std::uint8_t>(data.begin(), data.end())));
    const fs::path with_default = dir.file("default.png");
    const fs::path with_custom = dir.file("custom.png");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(with_default), "-w", "4"}) == 0);
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(with_custom), "-w", "4", "--font", utf8(font)}) == 0);
    const Decoded a = decode(with_default);
    const Decoded b = decode(with_custom);
    VGLYPH_CHECK(a.frames.size() == 1 && b.frames.size() == 1);
    if (a.frames.size() == 1 && b.frames.size() == 1) {
        VGLYPH_CHECK(a.frames[0].rgb24 == b.frames[0].rgb24);
    }

    const fs::path junk = dir.file("junk.ttf");
    VGLYPH_CHECK(write_file(junk, std::vector<std::uint8_t>(256, 'x')));
    const fs::path huge = dir.file("huge.ttf");
    VGLYPH_CHECK(write_file(huge, {}));
    std::error_code ec;
    fs::resize_file(huge, 16 * 1024 * 1024 + 1, ec);
    VGLYPH_CHECK(!ec);
    const fs::path out = dir.file("bad.png");
    for (const fs::path& bad : {junk, huge, dir.file("missing.ttf"), dir.file(".")}) {
        VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "--font", utf8(bad)}) == 1);
        VGLYPH_CHECK(!fs::exists(out) && no_temp(out));
    }
    // A codepoint the font lacks (U+4E00, a CJK ideograph).
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "--charset", " \xE4\xB8\x80"}) == 1);
    VGLYPH_CHECK(!fs::exists(out));
}

VGLYPH_TEST(the_output_may_be_the_input) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path png = white_png(dir);
    VGLYPH_CHECK(run_cli({"convert", utf8(png), "-o", utf8(png), "-w", "4"}) == 0);
    VGLYPH_CHECK(is_size(decode(png), 4 * kCellW, 2 * kCellH));

    const fs::path gif = bw_gif(dir);
    VGLYPH_CHECK(run_cli({"convert", utf8(gif), "-o", utf8(gif), "-w", "4"}) == 0);
    const Decoded d = decode(gif);
    VGLYPH_CHECK(is_size(d, 4 * kCellW, 2 * kCellH) && d.frames.size() == 3);
}

VGLYPH_TEST(a_failed_export_leaves_existing_files_alone) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = white_png(dir);
    const fs::path out = dir.file("out.png");
    const std::vector<std::uint8_t> old = {'o', 'l', 'd'};
    std::vector<std::uint8_t> now;

    // Over the output size cap (2048 cells of a 64 px font).
    VGLYPH_CHECK(write_file(out, old));
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "2048", "--font-size", "64"}) == 1);
    VGLYPH_CHECK(read_file(out, now) && now == old && no_temp(out));

    // A file that has the temporary name is not ours to overwrite or remove.
    fs::path temp = out;
    temp += ".vglyph-tmp";
    VGLYPH_CHECK(write_file(temp, old));
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4"}) == 1);
    VGLYPH_CHECK(read_file(temp, now) && now == old);
    VGLYPH_CHECK(read_file(out, now) && now == old);

    // The export is complete but cannot be renamed over a directory.
    const fs::path taken = dir.file("taken.png");
    std::error_code ec;
    fs::create_directories(taken / "keep", ec);
    VGLYPH_CHECK(!ec);
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(taken), "-w", "4"}) == 1);
    VGLYPH_CHECK(fs::exists(taken / "keep") && no_temp(taken));
}

VGLYPH_TEST(convert_png_to_txt) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = white_png(dir);
    const fs::path out = dir.file("out.txt");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono"}) == 0);
    // White maps to the brightest glyph; the file ends with a newline.
    VGLYPH_CHECK(read_text(out) == "@@@@\n@@@@\n");
}

VGLYPH_TEST(convert_png_to_ans_restarts_colors_on_each_row) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = white_png(dir);
    const fs::path out = dir.file("out.ans");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono"}) == 0);
    const std::string row =
        "\x1b[38;2;255;255;255m"
        "\x1b[48;2;0;0;0m"
        "@@@@"
        "\x1b[0m";
    VGLYPH_CHECK(read_text(out) == row + "\n" + row + "\n");
}

VGLYPH_TEST(convert_halfblock_uses_the_block_glyph) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = white_png(dir);
    const fs::path out = dir.file("out.txt");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--mode", "halfblock"}) == 0);
    const std::string row = "\xE2\x96\x80\xE2\x96\x80\xE2\x96\x80\xE2\x96\x80\n";
    VGLYPH_CHECK(read_text(out) == row + row);
}

VGLYPH_TEST(convert_sobel_draws_strokes_the_charset_does_not_hold) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = dir.file("step.png");
    std::vector<std::uint8_t> rgb = solid_rgb(8, 8, kBlack);
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        if ((i / 3) % 8 >= 4) {
            rgb[i] = 255;
        }
    }
    VGLYPH_CHECK(write_png(in, 8, 8, rgb).ok());
    const fs::path txt = dir.file("out.txt");
    const fs::path png = dir.file("out.png");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(txt), "-w", "4", "--mode", "sobel",
                          "--charset", " #"}) == 0);
    VGLYPH_CHECK(read_text(txt) == " ||#\n ||#\n");
    // The glyph atlas must hold the strokes too.
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(png), "-w", "4", "--mode", "sobel",
                          "--charset", " #"}) == 0);
}

VGLYPH_TEST(convert_braille_writes_text_and_draws_dots) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = dir.file("step.png");
    std::vector<std::uint8_t> rgb = solid_rgb(8, 8, kBlack);
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        if ((i / 3) % 8 < 4) {
            rgb[i] = 255;
        }
    }
    VGLYPH_CHECK(write_png(in, 8, 8, rgb).ok());
    const fs::path txt = dir.file("out.txt");
    const fs::path png = dir.file("out.png");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(txt), "-w", "4", "--mode", "braille"}) == 0);
    // Two U+28FF (all dots) then two U+2800 (none), as UTF-8.
    const std::string row = "\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA0\x80\xE2\xA0\x80\n";
    VGLYPH_CHECK(read_text(txt) == row + row);

    // The default font has no Braille glyphs: the dots are drawn without it.
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(png), "-w", "4", "--mode", "braille",
                          "--color", "mono"}) == 0);
    const Decoded d = decode(png);
    VGLYPH_CHECK(is_size(d, 4 * kCellW, 2 * kCellH));
    VGLYPH_CHECK(d.frames.size() == 1);
    if (d.frames.size() == 1 && is_size(d, 4 * kCellW, 2 * kCellH)) {
        const auto red_at = [&](std::size_t x, std::size_t y) {
            return d.frames.front().rgb24[(y * 4 * kCellW + x) * 3];
        };
        VGLYPH_CHECK(red_at(1, 1) == 255);   // inside the first dot
        VGLYPH_CHECK(red_at(0, 0) == 0);     // the gap around it
        VGLYPH_CHECK(red_at(21, 1) == 0);    // the unraised right half
    }
}

VGLYPH_TEST(convert_selects_the_requested_gif_frame) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = bw_gif(dir);
    const fs::path out = dir.file("out.txt");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono"}) == 0);
    VGLYPH_CHECK(read_text(out) == "    \n    \n");  // frame 0 is black

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono", "--frame", "1"}) == 0);
    VGLYPH_CHECK(read_text(out) == "@@@@\n@@@@\n");  // frame 1 is white

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "4", "--color", "mono", "--frame", "2"}) == 0);
    VGLYPH_CHECK(read_text(out) == "    \n    \n");
}

VGLYPH_TEST(frame_past_the_end_is_a_usage_error_and_writes_nothing) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = bw_gif(dir);
    const fs::path out = dir.file("out.txt");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "--frame", "3"}) == 2);
    VGLYPH_CHECK(!fs::exists(out));
}

VGLYPH_TEST(failures_use_the_documented_exit_codes_and_write_nothing) {
    const TempDir dir;
    VGLYPH_CHECK(dir.ok());
    const fs::path in = white_png(dir);
    const fs::path out = dir.file("out.txt");

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(dir.file("out.gif"))}) == 2);  // a photo is no animation
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "3"}) == 2);       // out of range
    VGLYPH_CHECK(run_cli({"convert", utf8(dir.file("missing.png")), "-o", utf8(out)}) == 1);
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(dir.file("no-such-dir") / "out.txt")}) == 1);
    VGLYPH_CHECK(run_cli({"play", utf8(in)}) == 2);  // the test runs without a terminal
    VGLYPH_CHECK(!fs::exists(out));
    VGLYPH_CHECK(!fs::exists(dir.file("out.gif")));
}

#ifndef _WIN32  // raise() does not reach SetConsoleCtrlHandler
namespace {

void prior_handler(int) {}

// Installs a guard, raises `signal`, and returns whether the guard recorded it.
bool guard_records(int signal) {
    const auto installed = InterruptGuard::install();
    const InterruptGuard* guard = installed.value_if();
    VGLYPH_CHECK(guard != nullptr);
    if (guard == nullptr) {
        return false;
    }
    VGLYPH_CHECK(!guard->requested());
    VGLYPH_CHECK(std::raise(signal) == 0);
    return guard->requested();
}

}  // namespace

VGLYPH_TEST(interrupt_guard_records_signals_and_restores_the_handlers) {
    for (const int signal : {SIGINT, SIGTERM, SIGHUP}) {
        const auto before = std::signal(signal, prior_handler);
        VGLYPH_CHECK(before != SIG_ERR);
        VGLYPH_CHECK(guard_records(signal));
        VGLYPH_CHECK(std::signal(signal, before) == prior_handler);
    }
}

VGLYPH_TEST(interrupt_guard_leaves_an_ignored_hangup_ignored) {
    const auto before = std::signal(SIGHUP, SIG_IGN);  // what nohup sets up
    VGLYPH_CHECK(before != SIG_ERR);
    VGLYPH_CHECK(!guard_records(SIGHUP));
    VGLYPH_CHECK(std::signal(SIGHUP, before) == SIG_IGN);
}
#endif

VGLYPH_TEST(exit_code_mapping) {
    VGLYPH_CHECK(exit_code(ErrorCode::Ok) == 0);
    VGLYPH_CHECK(exit_code(ErrorCode::InvalidArgument) == 2);
    VGLYPH_CHECK(exit_code(ErrorCode::Cancelled) == 130);
    VGLYPH_CHECK(exit_code(ErrorCode::IoError) == 1);
    VGLYPH_CHECK(exit_code(ErrorCode::UnsupportedFormat) == 1);
    VGLYPH_CHECK(exit_code(ErrorCode::DecodeError) == 1);
    VGLYPH_CHECK(exit_code(ErrorCode::LimitExceeded) == 1);
    VGLYPH_CHECK(exit_code(ErrorCode::InternalError) == 1);
}

VGLYPH_TEST(info_prints_one_key_value_line_per_field) {
    vglyph::MediaInfo info;
    VGLYPH_CHECK(format_info(info) ==
                 "kind: image\nwidth: 0\nheight: 0\npixel_format: \ncodec: \n"
                 "fps: 0.000\nduration_us: 0\nhas_audio: false\n");

    info.media_kind = vglyph::MediaKind::Video;
    info.src_width = 1920;
    info.src_height = 1080;
    info.pixel_format_name = "yuv420p";
    info.codec_name = "h264";
    info.avg_fps = 29.97;
    info.duration_us = 12'345'678;
    info.has_audio = true;
    VGLYPH_CHECK(format_info(info) ==
                 "kind: video\nwidth: 1920\nheight: 1080\npixel_format: yuv420p\ncodec: h264\n"
                 "fps: 29.970\nduration_us: 12345678\nhas_audio: true\n");

    info.media_kind = vglyph::MediaKind::AnimatedImage;
    VGLYPH_CHECK(format_info(info).starts_with("kind: animated-image\n"));
}

VGLYPH_TEST_MAIN()
