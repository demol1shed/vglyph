#pragma once

// The only CLI code that touches OS console APIs. platform_posix.cpp and
// platform_win32.cpp implement it; CMake builds the one that fits.

#include <memory>
#include <string_view>

#include "vglyph/error.hpp"

namespace vglyph::cli {

// RAII guard for the process's console state. On Windows it switches an
// attached console to UTF-8 and VT processing so .ans output renders, and
// restores both on destruction. A no-op on POSIX.
class ConsoleOutput {
public:
    ConsoleOutput();
    ~ConsoleOutput();
    ConsoleOutput(const ConsoleOutput&) = delete;
    ConsoleOutput& operator=(const ConsoleOutput&) = delete;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Writes every byte, unmodified (no newline translation), to stdout or
// stderr. Stdout is never touched through <cstdio>, so bytes cannot reorder.
[[nodiscard]] Status write_stdout(std::string_view bytes);
[[nodiscard]] Status write_stderr(std::string_view bytes);

}  // namespace vglyph::cli
