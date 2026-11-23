#pragma once

// UTF-8 encoding and strict decoding (RFC 3629). No exceptions.

#include <string>
#include <string_view>

#include "vglyph/error.hpp"

namespace vglyph {

// Appends `c` as UTF-8. A surrogate or a value past U+10FFFF is not a Unicode
// scalar value and is written as U+FFFD instead, so the output is always
// well-formed UTF-8. Allocates only if `out` has to grow.
void append_utf8(char32_t c, std::string& out);

// Decodes `text`. Overlong forms, surrogates, values past U+10FFFF and
// truncated or stray bytes fail with InvalidArgument.
[[nodiscard]] Result<std::u32string> decode_utf8(std::string_view text);

}  // namespace vglyph
