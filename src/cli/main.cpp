#include <cstdio>
#include <cstring>

#include "vglyph/version.hpp"

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        if (std::printf("vglyph %s\nLicense: GPLv3\n", vglyph::version_string()) < 0) {
            return 1;
        }
        return 0;
    }
    if (std::fprintf(stderr, "usage: vglyph --version\n") < 0) {
        return 1;
    }
    return 2;
}
