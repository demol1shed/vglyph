#include "args.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "vglyph/utf8.hpp"

namespace vglyph::cli {

namespace {

enum class Opt {
    Width,
    Height,
    Aspect,
    Mode,
    Color,
    Charset,
    Dither,
    Brightness,
    Contrast,
    Gamma,
    Invert,
    Loop,
    Frame,
    Output,
    Format,
    Font,
    FontSize,
    Crf,
    Help,
};

// Which commands accept an option.
constexpr unsigned kConvert = 1U << 0;
constexpr unsigned kPlay = 1U << 1;
constexpr unsigned kInfo = 1U << 2;

struct OptionSpec {
    std::string_view long_name;
    std::string_view short_name;  // empty when there is none
    Opt opt;
    bool takes_value;
    unsigned commands;
};

constexpr unsigned kRender = kConvert | kPlay;
constexpr std::array<OptionSpec, 19> kOptions{{
    {"--width", "-w", Opt::Width, true, kRender},
    {"--height", "-H", Opt::Height, true, kRender},
    {"--aspect", "", Opt::Aspect, true, kRender},
    {"--mode", "", Opt::Mode, true, kRender},
    {"--color", "", Opt::Color, true, kRender},
    {"--charset", "", Opt::Charset, true, kRender},
    {"--dither", "", Opt::Dither, true, kRender},
    {"--brightness", "", Opt::Brightness, true, kRender},
    {"--contrast", "", Opt::Contrast, true, kRender},
    {"--gamma", "", Opt::Gamma, true, kRender},
    {"--invert", "", Opt::Invert, false, kRender},
    {"--loop", "", Opt::Loop, false, kPlay},
    {"--frame", "", Opt::Frame, true, kConvert},
    {"--output", "-o", Opt::Output, true, kConvert},
    {"--format", "", Opt::Format, true, kConvert},
    {"--font", "", Opt::Font, true, kConvert},
    {"--font-size", "", Opt::FontSize, true, kConvert},
    {"--crf", "", Opt::Crf, true, kConvert},
    {"--help", "-h", Opt::Help, false, kConvert | kPlay | kInfo},
}};

[[nodiscard]] Error usage_error(std::string detail) {
    return make_error(ErrorCode::InvalidArgument, std::move(detail));
}

[[nodiscard]] std::string quoted(std::string_view s) {
    return "'" + std::string(s) + "'";
}

[[nodiscard]] Error not_implemented(std::string_view what) {
    return usage_error(std::string(what) + " is not implemented yet");
}

[[nodiscard]] Error bad_value(std::string_view option, std::string_view value,
                              std::string_view expected) {
    return usage_error("invalid value " + quoted(value) + " for " + std::string(option) +
                       " (expected " + std::string(expected) + ")");
}

// Whole-string number parse. from_chars never throws and rejects a sign it
// cannot store (so "-5" is not a uint32_t) and trailing junk is caught here.
template <class T>
[[nodiscard]] bool parse_number(std::string_view text, T& out) {
    T value{};
    const char* const end = text.data() + text.size();
    const std::from_chars_result r = std::from_chars(text.data(), end, value);
    if (text.empty() || r.ec != std::errc{} || r.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] const OptionSpec* find_option(std::string_view name) {
    for (const OptionSpec& spec : kOptions) {
        if (name == spec.long_name || (!spec.short_name.empty() && name == spec.short_name)) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] unsigned command_bit(Command c) {
    switch (c) {
        case Command::Convert:
            return kConvert;
        case Command::Play:
            return kPlay;
        case Command::Info:
            return kInfo;
        case Command::Help:
        case Command::Version:
            break;
    }
    return 0;
}

[[nodiscard]] std::string lower_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

// Everything parsed that is not stored in CliOptions itself.
struct ParseState {
    std::optional<OutputFormat> format;  // from --format
    bool have_output = false;
    bool have_input = false;
    // Given explicitly, even with the default value: some formats reject them.
    bool have_frame = false;
    bool have_font_size = false;
    bool have_crf = false;
};

// An output format and its name, which is also its file extension.
struct FormatName {
    std::string_view name;
    OutputFormat format;
};

constexpr std::array<FormatName, 5> kFormats{{
    {"txt", OutputFormat::Txt},
    {"ans", OutputFormat::Ans},
    {"png", OutputFormat::Png},
    {"gif", OutputFormat::Gif},
    {"mp4", OutputFormat::Mp4},
}};

[[nodiscard]] std::optional<OutputFormat> find_format(std::string_view name) {
    for (const FormatName& entry : kFormats) {
        if (name == entry.name) {
            return entry.format;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Status parse_float_option(std::string_view name, std::string_view value, float& out) {
    if (!parse_number(value, out)) {
        return bad_value(name, value, "a number");
    }
    return Status::success();
}

[[nodiscard]] Status apply(const OptionSpec& spec, std::string_view value, CliOptions& o,
                           ParseState& state) {
    ConvertConfig& c = o.config;
    switch (spec.opt) {
        case Opt::Width:
            if (!parse_number(value, c.target_cols)) {
                return bad_value(spec.long_name, value, "an integer 4..2048");
            }
            o.width_given = true;
            return Status::success();
        case Opt::Height:
            if (!parse_number(value, c.target_rows)) {
                return bad_value(spec.long_name, value, "an integer 0..1024");
            }
            o.height_given = true;
            return Status::success();
        case Opt::Aspect:
            o.aspect_given = true;
            return parse_float_option(spec.long_name, value, c.cell_aspect);
        case Opt::Brightness:
            return parse_float_option(spec.long_name, value, c.brightness);
        case Opt::Contrast:
            return parse_float_option(spec.long_name, value, c.contrast);
        case Opt::Gamma:
            return parse_float_option(spec.long_name, value, c.gamma);
        case Opt::Mode:
            if (value == "ascii") {
                c.render_mode = RenderMode::Ascii;
            } else if (value == "halfblock") {
                c.render_mode = RenderMode::HalfBlock;
            } else if (value == "sobel") {
                c.render_mode = RenderMode::SobelAscii;
            } else if (value == "braille") {
                return not_implemented("--mode braille");
            } else {
                return bad_value(spec.long_name, value, "ascii, halfblock, braille, sobel");
            }
            return Status::success();
        case Opt::Color:
            if (value == "mono") {
                c.color_mode = ColorMode::Mono;
            } else if (value == "gray") {
                c.color_mode = ColorMode::Grayscale;
            } else if (value == "fg") {
                c.color_mode = ColorMode::TrueColorFg;
            } else if (value == "fgbg") {
                c.color_mode = ColorMode::TrueColorFgBg;
            } else {
                return bad_value(spec.long_name, value, "mono, gray, fg, fgbg");
            }
            return Status::success();
        case Opt::Dither:
            if (value == "none") {
                c.dither_mode = DitherMode::None;
            } else if (value == "bayer" || value == "floyd") {
                return not_implemented("--dither " + std::string(value));
            } else {
                return bad_value(spec.long_name, value, "none, bayer, floyd");
            }
            return Status::success();
        case Opt::Charset: {
            Result<std::u32string> decoded = decode_utf8(value);
            std::u32string* charset = decoded.value_if();
            if (charset == nullptr) {
                return usage_error("--charset is not valid UTF-8");
            }
            c.charset = std::move(*charset);
            return Status::success();
        }
        case Opt::Invert:
            c.invert = true;
            return Status::success();
        case Opt::Loop:
            o.loop = true;
            return Status::success();
        case Opt::Frame:
            if (!parse_number(value, o.frame_index)) {
                return bad_value(spec.long_name, value, "a non-negative integer");
            }
            state.have_frame = true;
            return Status::success();
        case Opt::Output:
            state.have_output = true;
            o.to_stdout = value == "-";
            o.output = o.to_stdout ? std::filesystem::path() : path_from_utf8(value);
            return Status::success();
        case Opt::Format:
            state.format = find_format(value);
            if (!state.format.has_value()) {
                return bad_value(spec.long_name, value, "txt, ans, png, gif, mp4");
            }
            return Status::success();
        case Opt::Font:
            if (value.empty()) {
                return bad_value(spec.long_name, value, "a .ttf file");
            }
            o.font = path_from_utf8(value);
            return Status::success();
        case Opt::FontSize:
            if (!parse_number(value, o.font_px) || o.font_px < kMinFontPx || o.font_px > kMaxFontPx) {
                return bad_value(spec.long_name, value, "an integer 8..64");
            }
            state.have_font_size = true;
            return Status::success();
        case Opt::Crf:
            if (!parse_number(value, o.crf) || o.crf < kMinCrf || o.crf > kMaxCrf) {
                return bad_value(spec.long_name, value, "an integer 0..51");
            }
            state.have_crf = true;
            return Status::success();
        case Opt::Help:
            break;  // handled by the caller before apply()
    }
    return make_error(ErrorCode::InternalError, "unhandled option");
}

// The output format: --format, else the file extension, else .ans for stdout.
[[nodiscard]] Result<OutputFormat> resolve_format(const CliOptions& o, const ParseState& state) {
    if (state.format.has_value()) {
        return *state.format;
    }
    if (o.to_stdout) {
        return OutputFormat::Ans;
    }
    const std::u8string ext8 = o.output.extension().u8string();
    const std::string ext =
        lower_ascii(std::string(reinterpret_cast<const char*>(ext8.data()), ext8.size()));
    if (!ext.empty()) {
        if (const auto format = find_format(std::string_view(ext).substr(1))) {
            return *format;
        }
    }
    return usage_error(
        "cannot infer the output format from the file name; use --format txt|ans|png|gif|mp4");
}

// Options that only some output formats accept. Checked after parsing, because
// the format may be named after them.
[[nodiscard]] Status check_format_options(const CliOptions& o, const ParseState& state) {
    if (o.to_stdout && is_visual(o.format)) {
        return usage_error("'-o -' writes only txt or ans; png, gif and mp4 need a file");
    }
    if (!is_visual(o.format) && (o.font.has_value() || state.have_font_size)) {
        return usage_error("--font and --font-size apply only to png, gif and mp4 output");
    }
    if (state.have_crf && o.format != OutputFormat::Mp4) {
        return usage_error("--crf applies only to mp4 output");
    }
    if (state.have_frame && (o.format == OutputFormat::Gif || o.format == OutputFormat::Mp4)) {
        return usage_error("--frame applies only to txt, ans and png output");
    }
    return Status::success();
}

}  // namespace

std::filesystem::path path_from_utf8(std::string_view utf8) {
    const std::u8string u8(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(u8);
}

std::string_view usage_text() noexcept {
    return "Usage:\n"
           "  vglyph convert <input> -o <output> [options]\n"
           "  vglyph play <input> [options]\n"
           "  vglyph info <input>\n"
           "  vglyph --version | --help\n"
           "\n"
           "Commands:\n"
           "  convert   Write a photo, GIF or video as .txt, .ans, .png, .gif or .mp4\n"
           "            ('-o -' writes .txt or .ans to stdout)\n"
           "  play      Play a photo, GIF or video in the terminal\n"
           "  info      Print media metadata\n"
           "\n"
           "Options for convert and play:\n"
           "  -w, --width <cols>      Grid width in cells, 4..2048 (convert: 100)\n"
           "  -H, --height <rows>     Grid height in cells, 0..1024 (0 = automatic)\n"
           "      --aspect <float>    Cell width / height, 0.1..4.0 (0.5;\n"
           "                          png, gif, mp4: the font's cell shape)\n"
           "      --mode <mode>       ascii, halfblock, sobel (braille: not yet)\n"
           "      --color <mode>      mono, gray, fg, fgbg (fg)\n"
           "      --charset <string>  Dark-to-bright glyph ramp, at least 2 glyphs\n"
           "      --dither <mode>     none (bayer, floyd: not yet)\n"
           "      --brightness <f>    -1.0..1.0 (0.0)\n"
           "      --contrast <f>      0.0..4.0 (1.0)\n"
           "      --gamma <f>         0.1..4.0 (1.0)\n"
           "      --invert            Invert the luminance ramp\n"
           "\n"
           "Options for convert:\n"
           "  -o, --output <file>     Output file, or '-' for stdout (required)\n"
           "      --format <fmt>      txt, ans, png, gif or mp4\n"
           "                          (default: from the extension; ans for stdout)\n"
           "      --frame <index>     Frame for txt, ans or png, 0-based (0)\n"
           "      --font <path>       TrueType font for png, gif, mp4 (embedded font)\n"
           "      --font-size <px>    Font size for png, gif, mp4, 8..64 (16)\n"
           "      --crf <int>         mp4 quality, 0 (best) to 51 (20)\n"
           "\n"
           "Options for play:\n"
           "      --loop              Loop GIF and video playback\n"
           "  Keys: Space pauses, l toggles looping, q or Esc quits.\n";
}

Result<CliOptions> parse_args(std::span<const std::string_view> args) {
    if (args.empty()) {
        return usage_error("missing command");
    }

    CliOptions o;
    const std::string_view first = args[0];
    if (first == "--help" || first == "-h") {
        o.command = Command::Help;
        return o;
    }
    if (first == "--version") {
        o.command = Command::Version;
        return o;
    }
    if (first == "convert") {
        o.command = Command::Convert;
    } else if (first == "play") {
        o.command = Command::Play;
    } else if (first == "info") {
        o.command = Command::Info;
    } else {
        return usage_error("unknown command " + quoted(first));
    }

    ParseState state;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg.empty() || arg[0] != '-' || arg == "-") {
            if (state.have_input) {
                return usage_error("unexpected argument " + quoted(arg));
            }
            state.have_input = true;
            o.input = path_from_utf8(arg);
            continue;
        }

        // "--name=value" for long options, otherwise the value is the next argument.
        std::string_view name = arg;
        std::string_view inline_value;
        bool has_inline_value = false;
        if (arg.starts_with("--")) {
            const std::size_t eq = arg.find('=');
            if (eq != std::string_view::npos) {
                name = arg.substr(0, eq);
                inline_value = arg.substr(eq + 1);
                has_inline_value = true;
            }
        }

        const OptionSpec* spec = find_option(name);
        if (spec == nullptr || (spec->commands & command_bit(o.command)) == 0) {
            return usage_error("unknown option " + quoted(name) + " for " + std::string(first));
        }
        if (spec->opt == Opt::Help) {
            o.command = Command::Help;
            return o;
        }

        std::string_view value;
        if (!spec->takes_value) {
            if (has_inline_value) {
                return usage_error("option " + quoted(name) + " takes no value");
            }
        } else if (has_inline_value) {
            value = inline_value;
        } else if (i + 1 < args.size()) {
            value = args[++i];
        } else {
            return usage_error("option " + quoted(name) + " needs a value");
        }
        if (const Status s = apply(*spec, value, o, state); !s.ok()) {
            return s.error();
        }
    }

    if (!state.have_input) {
        return usage_error("missing input file");
    }
    if (o.command == Command::Convert) {
        if (!state.have_output) {
            return usage_error("missing -o <output>");
        }
        const Result<OutputFormat> format = resolve_format(o, state);
        if (format.error_if() != nullptr) {
            return *format.error_if();
        }
        o.format = *format.value_if();
        if (const Status s = check_format_options(o, state); !s.ok()) {
            return s.error();
        }
    }
    if (o.command != Command::Info) {
        if (const Status s = validate(o.config); !s.ok()) {
            return s.error();
        }
    }
    return o;
}

}  // namespace vglyph::cli
