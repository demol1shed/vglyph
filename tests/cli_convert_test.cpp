#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "commands.hpp"
#include "media_fixture.hpp"
#include "test_framework.hpp"

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

}  // namespace

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

    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(dir.file("out.png"))}) == 2);  // not implemented
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(out), "-w", "3"}) == 2);       // out of range
    VGLYPH_CHECK(run_cli({"convert", utf8(dir.file("missing.png")), "-o", utf8(out)}) == 1);
    VGLYPH_CHECK(run_cli({"convert", utf8(in), "-o", utf8(dir.file("no-such-dir") / "out.txt")}) == 1);
    VGLYPH_CHECK(run_cli({"play", utf8(in)}) == 2);  // play arrives in a later PR
    VGLYPH_CHECK(!fs::exists(out));
    VGLYPH_CHECK(!fs::exists(dir.file("out.png")));
}

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
