#include <cstdio>

#include "vglyph/version.hpp"

int main() {
    if (std::printf("vglyph-gui %s (stub)\n", vglyph::version_string()) < 0) {
        return 1;
    }
    return 0;
}
