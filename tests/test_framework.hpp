#pragma once

// Minimal single-header test framework. No exceptions, no RTTI.
// A failed CHECK records the failure and the test continues.

#include <cstdio>
#include <vector>

namespace vglyph_test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all() {
    for (const TestCase& t : registry()) {
        const int before = failure_count();
        t.fn();
        const bool ok = failure_count() == before;
        (void)std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
    }
    (void)std::printf("%zu test(s), %d failure(s)\n", registry().size(), failure_count());
    return failure_count() == 0 ? 0 : 1;
}

}  // namespace vglyph_test

#define VGLYPH_TEST(name)                                                  \
    static void name();                                                    \
    static const vglyph_test::Registrar name##_registrar(#name, &name);    \
    static void name()

#define VGLYPH_CHECK(expr)                                                         \
    do {                                                                           \
        if (!(expr)) {                                                             \
            ++vglyph_test::failure_count();                                        \
            (void)std::printf("  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        }                                                                          \
    } while (false)

#define VGLYPH_TEST_MAIN() \
    int main() { return vglyph_test::run_all(); }
