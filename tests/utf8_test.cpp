#include <string>

#include "test_framework.hpp"
#include "vglyph/utf8.hpp"

using vglyph::ErrorCode;

namespace {

std::string encode(char32_t c) {
    std::string out;
    vglyph::append_utf8(c, out);
    return out;
}

bool rejects(const std::string& bytes) {
    const vglyph::Result<std::u32string> r = vglyph::decode_utf8(bytes);
    return r.error_if() != nullptr && r.error_if()->code == ErrorCode::InvalidArgument;
}

}  // namespace

VGLYPH_TEST(encode_uses_the_shortest_form) {
    VGLYPH_CHECK(encode(U'A') == "A");
    VGLYPH_CHECK(encode(0x7F) == "\x7F");
    VGLYPH_CHECK(encode(0x80) == "\xC2\x80");
    VGLYPH_CHECK(encode(0xE9) == "\xC3\xA9");
    VGLYPH_CHECK(encode(0x7FF) == "\xDF\xBF");
    VGLYPH_CHECK(encode(0x800) == "\xE0\xA0\x80");
    VGLYPH_CHECK(encode(0x2580) == "\xE2\x96\x80");
    VGLYPH_CHECK(encode(0xFFFF) == "\xEF\xBF\xBF");
    VGLYPH_CHECK(encode(0x10000) == "\xF0\x90\x80\x80");
    VGLYPH_CHECK(encode(0x10FFFF) == "\xF4\x8F\xBF\xBF");
}

VGLYPH_TEST(encode_replaces_non_scalar_values) {
    const std::string replacement = "\xEF\xBF\xBD";  // U+FFFD
    VGLYPH_CHECK(encode(0xD800) == replacement);
    VGLYPH_CHECK(encode(0xDFFF) == replacement);
    VGLYPH_CHECK(encode(0x110000) == replacement);
}

VGLYPH_TEST(encode_appends_without_clearing) {
    std::string out = "x";
    vglyph::append_utf8(0xE9, out);
    VGLYPH_CHECK(out == "x\xC3\xA9");
}

VGLYPH_TEST(decode_round_trips_every_length) {
    const std::u32string text = {U'A', 0xE9, 0x2580, 0x1F600, 0x10FFFF};
    std::string bytes;
    for (const char32_t c : text) {
        vglyph::append_utf8(c, bytes);
    }
    const vglyph::Result<std::u32string> r = vglyph::decode_utf8(bytes);
    const std::u32string* decoded = r.value_if();
    VGLYPH_CHECK(decoded != nullptr);
    VGLYPH_CHECK(decoded != nullptr && *decoded == text);
}

VGLYPH_TEST(decode_accepts_empty_input) {
    const vglyph::Result<std::u32string> r = vglyph::decode_utf8("");
    const std::u32string* decoded = r.value_if();
    VGLYPH_CHECK(decoded != nullptr && decoded->empty());
}

VGLYPH_TEST(decode_rejects_malformed_input) {
    VGLYPH_CHECK(rejects("\x80"));              // stray continuation byte
    VGLYPH_CHECK(rejects("\xFF"));              // never valid
    VGLYPH_CHECK(rejects("\xF5\x80\x80\x80"));  // lead byte past U+10FFFF
    VGLYPH_CHECK(rejects("\xC0\x80"));          // overlong NUL
    VGLYPH_CHECK(rejects("\xC1\xBF"));          // overlong 0x7F
    VGLYPH_CHECK(rejects("\xE0\x80\x80"));      // overlong three-byte form
    VGLYPH_CHECK(rejects("\xF0\x80\x80\x80"));  // overlong four-byte form
    VGLYPH_CHECK(rejects("\xED\xA0\x80"));      // U+D800 surrogate
    VGLYPH_CHECK(rejects("\xF4\x90\x80\x80"));  // U+110000
    VGLYPH_CHECK(rejects("\xE2\x96"));          // truncated at the end
    VGLYPH_CHECK(rejects("\xC3" "A"));          // continuation byte missing
    VGLYPH_CHECK(rejects("ok\xE2\x96" "x"));    // error after valid text
}

VGLYPH_TEST_MAIN()
