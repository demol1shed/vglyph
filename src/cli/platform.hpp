#pragma once

// The only CLI code that touches OS console APIs. platform_posix.cpp and
// platform_win32.cpp implement it; CMake builds the one that fits.

#include <chrono>
#include <cstdint>
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

struct TerminalSize {
    std::uint32_t cols = 0;
    std::uint32_t rows = 0;
};

enum class TerminalEvent {
    None,         // timed out, or a key the player does not use
    Resized,
    Interrupted,  // SIGINT / SIGTERM, or Ctrl+C / Ctrl+Break
    Quit,         // q, Esc, or the input is gone
    TogglePause,  // Space
    ToggleLoop,   // l
};

// RAII guard for interactive playback. open() puts the terminal in
// alternate-screen, hidden-cursor, unbuffered-key mode and installs the
// interrupt handlers; the destructor restores everything, so the terminal is
// back to normal on every way out of the player, including Ctrl+C. Signal
// keys keep working (the handlers only record the request), and "\n" still
// returns the carriage. One guard may exist at a time.
class Terminal {
public:
    // InvalidArgument when stdin or stdout is not a terminal.
    [[nodiscard]] static Result<Terminal> open();

    Terminal(Terminal&&) noexcept;
    Terminal& operator=(Terminal&&) = delete;
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    ~Terminal();

    // Current size of the terminal window in cells.
    [[nodiscard]] Result<TerminalSize> size() const;

    // Waits up to `timeout` for the next event and returns it, or None when
    // the time ran out or a key was ignored. It never waits longer than a
    // short internal cap, so a signal that lands just before the wait is still
    // noticed promptly; callers loop and re-check their own clock.
    // shortcut: Esc quits, so arrow keys (which start with Esc) quit on POSIX
    // too; upgrade by reading the rest of the escape sequence.
    [[nodiscard]] TerminalEvent wait(std::chrono::microseconds timeout);

private:
    struct State;
    explicit Terminal(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

// RAII guard for a long export. While one exists, Ctrl+C, SIGTERM and SIGHUP
// (Ctrl+C and Ctrl+Break on Windows) only record the request, so the caller can
// stop between frames and clean up. A SIGHUP that was already ignored (nohup)
// stays ignored. The guard shares that record with Terminal, so the two must not
// exist at the same time, and only one guard may exist at a time.
class InterruptGuard {
public:
    [[nodiscard]] static Result<InterruptGuard> install();

    InterruptGuard(InterruptGuard&&) noexcept;
    InterruptGuard& operator=(InterruptGuard&&) = delete;
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;
    ~InterruptGuard();

    // True once an interrupt arrived.
    [[nodiscard]] bool requested() const noexcept;

private:
    struct State;
    explicit InterruptGuard(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

// Writes every byte, unmodified (no newline translation), to stdout or
// stderr. Stdout is never touched through <cstdio>, so bytes cannot reorder.
[[nodiscard]] Status write_stdout(std::string_view bytes);
[[nodiscard]] Status write_stderr(std::string_view bytes);

}  // namespace vglyph::cli
