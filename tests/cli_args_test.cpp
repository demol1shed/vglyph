#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "args.hpp"
#include "test_framework.hpp"

using vglyph::ColorMode;
using vglyph::ErrorCode;
using vglyph::RenderMode;
using vglyph::Result;
using namespace vglyph::cli;

namespace {

Result<CliOptions> parse(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> list(args);
    return parse_args(list);
}

// The parse succeeded; returns the options (defaults on failure, after a recorded failure).
CliOptions ok(const Result<CliOptions>& r) {
    VGLYPH_CHECK(r.value_if() != nullptr);
    return r.value_if() != nullptr ? *r.value_if() : CliOptions{};
}

// The parse failed as a usage error whose message contains `needle`.
bool usage_error(const Result<CliOptions>& r, std::string_view needle = "") {
    const vglyph::Error* e = r.error_if();
    return e != nullptr && e->code == ErrorCode::InvalidArgument &&
           e->detail.find(needle) != std::string::npos;
}

}  // namespace

VGLYPH_TEST(convert_defaults) {
    const CliOptions o = ok(parse({"convert", "in.png", "-o", "out.txt"}));
    VGLYPH_CHECK(o.command == Command::Convert);
    VGLYPH_CHECK(o.input == "in.png");
    VGLYPH_CHECK(o.output == "out.txt");
    VGLYPH_CHECK(!o.to_stdout);
    VGLYPH_CHECK(o.format == OutputFormat::Txt);
    VGLYPH_CHECK(o.config.target_cols == 100);
    VGLYPH_CHECK(!o.width_given && !o.height_given);
    VGLYPH_CHECK(o.frame_index == 0);
    VGLYPH_CHECK(!o.loop);
}

VGLYPH_TEST(convert_reads_every_render_option) {
    const CliOptions o =
        ok(parse({"convert", "in.png", "-o", "out.ans", "-w", "80", "-H", "20", "--aspect", "0.4",
                  "--mode", "halfblock", "--color", "fgbg", "--charset", " .#", "--brightness",
                  "-0.25", "--contrast", "1.5", "--gamma", "2", "--invert", "--frame", "3"}));
    VGLYPH_CHECK(o.format == OutputFormat::Ans);
    VGLYPH_CHECK(o.config.target_cols == 80 && o.width_given);
    VGLYPH_CHECK(o.config.target_rows == 20 && o.height_given);
    VGLYPH_CHECK(o.config.cell_aspect == 0.4f);
    VGLYPH_CHECK(o.config.render_mode == RenderMode::HalfBlock);
    VGLYPH_CHECK(o.config.color_mode == ColorMode::TrueColorFgBg);
    VGLYPH_CHECK(o.config.charset == U" .#");
    VGLYPH_CHECK(o.config.brightness == -0.25f);
    VGLYPH_CHECK(o.config.contrast == 1.5f);
    VGLYPH_CHECK(o.config.gamma == 2.0f);
    VGLYPH_CHECK(o.config.invert);
    VGLYPH_CHECK(o.frame_index == 3);
}

VGLYPH_TEST(long_options_accept_equals_form) {
    const CliOptions o = ok(parse({"convert", "in.png", "--output=out.txt", "--width=64", "--color=mono"}));
    VGLYPH_CHECK(o.output == "out.txt");
    VGLYPH_CHECK(o.config.target_cols == 64);
    VGLYPH_CHECK(o.config.color_mode == ColorMode::Mono);
}

VGLYPH_TEST(output_format_comes_from_flag_extension_or_stdout) {
    VGLYPH_CHECK(ok(parse({"convert", "a", "-o", "o.txt"})).format == OutputFormat::Txt);
    VGLYPH_CHECK(ok(parse({"convert", "a", "-o", "o.ans"})).format == OutputFormat::Ans);
    VGLYPH_CHECK(ok(parse({"convert", "a", "-o", "o.TXT"})).format == OutputFormat::Txt);

    const CliOptions to_stdout = ok(parse({"convert", "a", "-o", "-"}));
    VGLYPH_CHECK(to_stdout.to_stdout && to_stdout.output.empty());
    VGLYPH_CHECK(to_stdout.format == OutputFormat::Ans);  // ANSI is the stdout default

    VGLYPH_CHECK(ok(parse({"convert", "a", "-o", "-", "--format", "txt"})).format == OutputFormat::Txt);
    VGLYPH_CHECK(ok(parse({"convert", "a", "-o", "o.dat", "--format", "txt"})).format == OutputFormat::Txt);
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.dat"}), "cannot infer"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "noext"}), "cannot infer"));
}

VGLYPH_TEST(unfinished_features_are_rejected_as_not_implemented) {
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--mode", "braille"}), "not implemented"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--mode", "sobel"}), "not implemented"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--dither", "bayer"}), "not implemented"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--dither", "floyd"}), "not implemented"));
}

VGLYPH_TEST(structural_errors) {
    VGLYPH_CHECK(usage_error(parse({}), "missing command"));
    VGLYPH_CHECK(usage_error(parse({"bogus"}), "unknown command"));
    VGLYPH_CHECK(usage_error(parse({"convert", "-o", "o.txt"}), "missing input"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a"}), "missing -o"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "b", "-o", "o.txt"}), "unexpected argument"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--frobnicate"}), "unknown option"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o"}), "needs a value"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--invert=1"}), "takes no value"));
    VGLYPH_CHECK(usage_error(parse({"play"}), "missing input"));
    VGLYPH_CHECK(usage_error(parse({"info"}), "missing input"));
}

VGLYPH_TEST(malformed_and_out_of_range_numbers_are_usage_errors) {
    const char* const bad[][2] = {
        {"-w", "abc"},          {"-w", "10x"},   {"-w", ""},        {"-w", "-5"},
        {"-w", "3"},            {"-w", "2049"},  {"-H", "1025"},    {"--aspect", "0.01"},
        {"--aspect", "x"},      {"--brightness", "1.1"},            {"--contrast", "-1"},
        {"--gamma", "5"},       {"--gamma", "nan"},                 {"--gamma", "inf"},
        {"--gamma", "1e999"},   {"--frame", "-1"},                  {"--frame", "x"},
        {"--mode", "cubist"},   {"--color", "rainbow"},             {"--dither", "dots"},
        {"--format", "pdf"},
    };
    for (const auto& pair : bad) {
        const std::vector<std::string_view> args = {"convert", "in.png", "-o", "o.txt", pair[0], pair[1]};
        VGLYPH_CHECK(usage_error(parse_args(args)));
    }
}

VGLYPH_TEST(range_boundaries_are_accepted) {
    VGLYPH_CHECK(ok(parse({"convert", "a", "-o", "o.txt", "-w", "4"})).config.target_cols == 4);
    VGLYPH_CHECK(ok(parse({"convert", "a", "-o", "o.txt", "-w", "2048", "-H", "1024"})).config.target_rows == 1024);
    VGLYPH_CHECK(ok(parse({"convert", "a", "-o", "o.txt", "-H", "0"})).height_given);
}

VGLYPH_TEST(charset_is_decoded_as_utf8_and_validated) {
    // 0x2580 (U+2580) then 'x'; hex escapes are split so they cannot swallow the 'x'.
    const CliOptions o = ok(parse({"convert", "a", "-o", "o.txt", "--charset", "\xE2\x96\x80" "x"}));
    VGLYPH_CHECK(o.config.charset == std::u32string({0x2580, U'x'}));

    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--charset", "\xC3"}), "UTF-8"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--charset", "a\x1b" "b"}), "charset"));
    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--charset", "x"}), "charset"));
}

VGLYPH_TEST(options_are_limited_to_their_commands) {
    const CliOptions p = ok(parse({"play", "in.gif", "--loop", "-w", "80"}));
    VGLYPH_CHECK(p.command == Command::Play && p.loop && p.width_given);
    VGLYPH_CHECK(!ok(parse({"play", "in.gif"})).width_given);

    VGLYPH_CHECK(usage_error(parse({"convert", "a", "-o", "o.txt", "--loop"}), "unknown option"));
    VGLYPH_CHECK(usage_error(parse({"play", "a", "--frame", "2"}), "unknown option"));
    VGLYPH_CHECK(usage_error(parse({"play", "a", "-o", "x"}), "unknown option"));
    VGLYPH_CHECK(usage_error(parse({"info", "a", "-w", "5"}), "unknown option"));

    const CliOptions i = ok(parse({"info", "a.mp4"}));
    VGLYPH_CHECK(i.command == Command::Info && i.input == "a.mp4");
}

VGLYPH_TEST(help_and_version) {
    VGLYPH_CHECK(ok(parse({"--help"})).command == Command::Help);
    VGLYPH_CHECK(ok(parse({"-h"})).command == Command::Help);
    VGLYPH_CHECK(ok(parse({"--version"})).command == Command::Version);
    VGLYPH_CHECK(ok(parse({"convert", "--help"})).command == Command::Help);  // no input needed
    VGLYPH_CHECK(ok(parse({"play", "a", "-h"})).command == Command::Help);
    VGLYPH_CHECK(!usage_text().empty());
}

VGLYPH_TEST(paths_are_built_from_utf8_bytes) {
    const std::filesystem::path p = path_from_utf8("\xC3\xA9.png");  // e-acute
    const std::u8string u8 = p.u8string();
    VGLYPH_CHECK(std::string(reinterpret_cast<const char*>(u8.data()), u8.size()) == "\xC3\xA9.png");
}

VGLYPH_TEST_MAIN()
