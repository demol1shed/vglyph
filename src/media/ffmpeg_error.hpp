#pragma once

// Internal helper (not installed): maps an FFmpeg status code to an Error.
// Deliberately free of FFmpeg headers, so callers include it without them.

#include "vglyph/error.hpp"

namespace vglyph {

// Builds "<what>: <av_strerror text> (<averr>)" under `code`. `averr` is the
// negative value an FFmpeg call returned; an unknown code still yields a
// usable message.
[[nodiscard]] Error ffmpeg_error(ErrorCode code, const char* what, int averr);

}  // namespace vglyph
