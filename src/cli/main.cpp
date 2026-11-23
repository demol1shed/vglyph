#include <string_view>
#include <vector>

#include "commands.hpp"
#include "platform.hpp"
#include "vglyph/media_reader.hpp"

int main(int argc, char** argv) {
    // FFmpeg's own stderr output would break SPEC 8.3's stream contract.
    vglyph::set_media_log_quiet();
    const vglyph::cli::ConsoleOutput console;

    std::vector<std::string_view> args;
    if (argc > 1) {
        args.assign(argv + 1, argv + argc);
    }
    return vglyph::cli::run(args);
}
