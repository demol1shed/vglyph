#include <cstring>

#include "test_framework.hpp"
#include "vglyph/version.hpp"

VGLYPH_TEST(version_string_is_not_empty) {
    VGLYPH_CHECK(vglyph::version_string() != nullptr);
    VGLYPH_CHECK(std::strlen(vglyph::version_string()) > 0);
}

VGLYPH_TEST(framework_reports_failures) {
    // Sanity: a true check must not bump the failure counter.
    const int before = vglyph_test::failure_count();
    VGLYPH_CHECK(1 + 1 == 2);
    VGLYPH_CHECK(vglyph_test::failure_count() == before);
}

VGLYPH_TEST_MAIN()
