#include "ffmpeg_error.hpp"

#include <array>
#include <string>

extern "C" {
#include <libavutil/error.h>
}

namespace vglyph {

Error ffmpeg_error(ErrorCode code, const char* what, int averr) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    const std::string reason = av_strerror(averr, text.data(), text.size()) < 0
                                   ? "unknown error"
                                   : std::string(text.data());
    return make_error(code, std::string(what) + ": " + reason + " (" + std::to_string(averr) + ")");
}

}  // namespace vglyph
