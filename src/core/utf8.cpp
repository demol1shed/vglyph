#include "vglyph/utf8.hpp"

#include <cstddef>
#include <cstdint>

namespace vglyph {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

[[nodiscard]] bool is_scalar_value(char32_t c) noexcept {
    return c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF);
}

[[nodiscard]] bool is_continuation(char byte) noexcept {
    return (static_cast<std::uint8_t>(byte) & 0xC0) == 0x80;
}

[[nodiscard]] char byte_of(char32_t value) noexcept { return static_cast<char>(value & 0xFF); }

}  // namespace

void append_utf8(char32_t c, std::string& out) {
    if (!is_scalar_value(c)) {
        c = kReplacement;
    }
    if (c < 0x80) {
        out.push_back(byte_of(c));
    } else if (c < 0x800) {
        out.push_back(byte_of(0xC0 | (c >> 6)));
        out.push_back(byte_of(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(byte_of(0xE0 | (c >> 12)));
        out.push_back(byte_of(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(byte_of(0x80 | (c & 0x3F)));
    } else {
        out.push_back(byte_of(0xF0 | (c >> 18)));
        out.push_back(byte_of(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(byte_of(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(byte_of(0x80 | (c & 0x3F)));
    }
}

Result<std::u32string> decode_utf8(std::string_view text) {
    const Error malformed = make_error(ErrorCode::InvalidArgument, "malformed UTF-8 input");
    std::u32string out;
    out.reserve(text.size());  // one codepoint per byte at most

    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[i]);
        std::size_t length = 0;
        char32_t value = 0;
        char32_t min_value = 0;  // smallest value that needs this length (rejects overlong forms)
        if (lead < 0x80) {
            length = 1;
            value = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            value = lead & 0x1Fu;
            min_value = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            value = lead & 0x0Fu;
            min_value = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            value = lead & 0x07u;
            min_value = 0x10000;
        } else {
            return malformed;  // stray continuation byte, C0/C1 (always overlong), F5..FF
        }
        if (text.size() - i < length) {
            return malformed;  // truncated sequence
        }
        for (std::size_t k = 1; k < length; ++k) {
            if (!is_continuation(text[i + k])) {
                return malformed;
            }
            value = (value << 6) | (static_cast<std::uint8_t>(text[i + k]) & 0x3Fu);
        }
        if (value < min_value || !is_scalar_value(value)) {
            return malformed;
        }
        out.push_back(value);
        i += length;
    }
    return out;
}

}  // namespace vglyph
