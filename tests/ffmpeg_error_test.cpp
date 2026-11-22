#include <cerrno>
#include <string>

#include "ffmpeg_error.hpp"
#include "test_framework.hpp"

using vglyph::ErrorCode;
using vglyph::ffmpeg_error;

VGLYPH_TEST(ffmpeg_error_keeps_code_and_names_the_call) {
    const vglyph::Error e = ffmpeg_error(ErrorCode::IoError, "avformat_open_input", -ENOENT);
    VGLYPH_CHECK(e.code == ErrorCode::IoError);
    VGLYPH_CHECK(e.detail.find("avformat_open_input: ") == 0);
    VGLYPH_CHECK(e.detail.find("(" + std::to_string(-ENOENT) + ")") != std::string::npos);
}

VGLYPH_TEST(ffmpeg_error_includes_av_strerror_text) {
    const vglyph::Error e = ffmpeg_error(ErrorCode::DecodeError, "decode", -ENOENT);
    VGLYPH_CHECK(e.detail.find("No such file or directory") != std::string::npos);
}

VGLYPH_TEST(ffmpeg_error_survives_unknown_code) {
    const vglyph::Error e = ffmpeg_error(ErrorCode::DecodeError, "decode", -123456789);
    VGLYPH_CHECK(e.code == ErrorCode::DecodeError);
    VGLYPH_CHECK(!e.detail.empty());
    VGLYPH_CHECK(e.detail.find("(-123456789)") != std::string::npos);
}

VGLYPH_TEST_MAIN()
