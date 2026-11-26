// windows.h must come after these defines and stays confined to this file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

#include "platform.hpp"

namespace vglyph::cli {

struct ConsoleOutput::State {
    HANDLE out = INVALID_HANDLE_VALUE;
    DWORD saved_mode = 0;
    UINT saved_code_page = 0;
    bool mode_changed = false;
    bool code_page_changed = false;
};

ConsoleOutput::ConsoleOutput() : state_(std::make_unique<State>()) {
    State& s = *state_;
    s.out = GetStdHandle(STD_OUTPUT_HANDLE);

    // GetConsoleMode only succeeds for a real console, not a pipe or a file.
    DWORD mode = 0;
    if (s.out != INVALID_HANDLE_VALUE && s.out != nullptr && GetConsoleMode(s.out, &mode) != 0) {
        if (SetConsoleMode(s.out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
            s.saved_mode = mode;
            s.mode_changed = true;
        }
    }

    // Also covers messages on stderr when only stdout is redirected.
    const UINT code_page = GetConsoleOutputCP();
    if (code_page != 0 && code_page != CP_UTF8 && SetConsoleOutputCP(CP_UTF8) != 0) {
        s.saved_code_page = code_page;
        s.code_page_changed = true;
    }
}

ConsoleOutput::~ConsoleOutput() {
    // Best effort: a destructor has nobody to report a failed restore to.
    if (state_->mode_changed) {
        static_cast<void>(SetConsoleMode(state_->out, state_->saved_mode));
    }
    if (state_->code_page_changed) {
        static_cast<void>(SetConsoleOutputCP(state_->saved_code_page));
    }
}

namespace {

// Console writes of 64 KiB or more can fail with ERROR_NOT_ENOUGH_MEMORY, so
// a frame goes out in chunks (inside one DEC 2026 update, so it still shows atomically).
constexpr std::size_t kMaxChunk = std::size_t{32} * 1024;

// WriteFile on a pipe or file writes the bytes as they are, so "\n" stays "\n".
[[nodiscard]] Status write_all(DWORD handle_id, std::string_view bytes, const char* stream) {
    const HANDLE handle = GetStdHandle(handle_id);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return make_error(ErrorCode::IoError, std::string(stream) + " is not available");
    }
    const char* p = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min(left, kMaxChunk));
        DWORD written = 0;
        if (WriteFile(handle, p, chunk, &written, nullptr) == 0 || written == 0) {
            return make_error(ErrorCode::IoError, std::string("write to ") + stream + " failed");
        }
        p += written;
        left -= written;
    }
    return Status::success();
}

}  // namespace

Status write_stdout(std::string_view bytes) {
    return write_all(STD_OUTPUT_HANDLE, bytes, "stdout");
}

Status write_stderr(std::string_view bytes) {
    return write_all(STD_ERROR_HANDLE, bytes, "stderr");
}

// ---- Terminal ---------------------------------------------------------------

namespace {

std::atomic<bool> g_stop{false};

// Runs on a thread of its own: it only records the request.
BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

// Alternate screen + hide cursor, and back. Leaving also ends a synchronized
// update that a Ctrl+C cut off (the tty drops queued output on a signal) and
// resets the colors.
constexpr std::string_view kEnterScreen = "\x1b[?1049h\x1b[?25l";
constexpr std::string_view kLeaveScreen = "\x1b[?2026l\x1b[0m\x1b[?25h\x1b[?1049l";
constexpr std::chrono::microseconds kMaxWait{100'000};

[[nodiscard]] bool usable(HANDLE handle) noexcept {
    return handle != INVALID_HANDLE_VALUE && handle != nullptr;
}

[[nodiscard]] Error win_error(const char* what) {
    return make_error(ErrorCode::IoError,
                      std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")");
}

}  // namespace

struct Terminal::State {
    HANDLE in = INVALID_HANDLE_VALUE;
    HANDLE out = INVALID_HANDLE_VALUE;
    DWORD saved_in_mode = 0;
    DWORD saved_out_mode = 0;
    bool in_changed = false;
    bool out_changed = false;
    bool handler = false;
    bool screen = false;
};

Terminal::Terminal(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Terminal::Terminal(Terminal&&) noexcept = default;

Terminal::~Terminal() {
    if (!state_) {
        return;  // moved from
    }
    // Best effort: a destructor has nobody to report a failed restore to.
    State& s = *state_;
    if (s.screen) {
        static_cast<void>(write_stdout(kLeaveScreen));
    }
    if (s.handler) {
        static_cast<void>(SetConsoleCtrlHandler(on_ctrl, FALSE));
    }
    if (s.out_changed) {
        static_cast<void>(SetConsoleMode(s.out, s.saved_out_mode));
    }
    if (s.in_changed) {
        static_cast<void>(SetConsoleMode(s.in, s.saved_in_mode));
    }
}

Result<Terminal> Terminal::open() {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0;
    DWORD out_mode = 0;
    if (!usable(in) || !usable(out) || GetConsoleMode(in, &in_mode) == 0 ||
        GetConsoleMode(out, &out_mode) == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "play requires an interactive terminal (stdin and stdout must be a console)");
    }
    // From here on, returning early destroys `term`, which undoes what was set up.
    Terminal term(std::make_unique<State>());
    State& s = *term.state_;
    s.in = in;
    s.out = out;

    // Keys arrive one by one, without echo; Ctrl+C stays a signal
    // (ENABLE_PROCESSED_INPUT) and resizes arrive as input records.
    const DWORD raw_in = (in_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                                        ENABLE_VIRTUAL_TERMINAL_INPUT)) |
                         ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT;
    if (SetConsoleMode(in, raw_in) == 0) {
        return win_error("SetConsoleMode(input)");
    }
    s.saved_in_mode = in_mode;
    s.in_changed = true;

    if (SetConsoleMode(out, out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
        return win_error("SetConsoleMode(output)");
    }
    s.saved_out_mode = out_mode;
    s.out_changed = true;

    g_stop.store(false);
    if (SetConsoleCtrlHandler(on_ctrl, TRUE) == 0) {
        return win_error("SetConsoleCtrlHandler");
    }
    s.handler = true;

    s.screen = true;
    if (const Status entered = write_stdout(kEnterScreen); !entered.ok()) {
        return entered.error();
    }
    return term;
}

Result<TerminalSize> Terminal::size() const {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(state_->out, &info) == 0) {
        return win_error("GetConsoleScreenBufferInfo");
    }
    // The visible window, not dwSize (which is the scrollback buffer).
    const int cols = info.srWindow.Right - info.srWindow.Left + 1;
    const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (cols <= 0 || rows <= 0) {
        return make_error(ErrorCode::IoError, "cannot read the terminal size");
    }
    return TerminalSize{static_cast<std::uint32_t>(cols), static_cast<std::uint32_t>(rows)};
}

TerminalEvent Terminal::wait(std::chrono::microseconds timeout) {
    if (g_stop.load()) {
        return TerminalEvent::Interrupted;
    }
    const auto bounded = std::clamp(timeout, std::chrono::microseconds::zero(), kMaxWait);
    const DWORD ms = static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(bounded).count());

    // Ctrl+C produces no input record, so it cannot wake this wait by itself.
    const DWORD waited = WaitForSingleObject(state_->in, ms);
    if (g_stop.load()) {
        return TerminalEvent::Interrupted;
    }
    if (waited == WAIT_TIMEOUT) {
        return TerminalEvent::None;
    }
    if (waited != WAIT_OBJECT_0) {
        return TerminalEvent::Quit;
    }

    // Check the queue first so that ReadConsoleInput cannot block.
    DWORD queued = 0;
    if (GetNumberOfConsoleInputEvents(state_->in, &queued) == 0) {
        return TerminalEvent::Quit;
    }
    if (queued == 0) {
        return TerminalEvent::None;
    }
    INPUT_RECORD record{};
    DWORD read = 0;
    if (ReadConsoleInputW(state_->in, &record, 1, &read) == 0) {
        return TerminalEvent::Quit;
    }
    if (read == 0) {
        return TerminalEvent::None;
    }

    if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
        return TerminalEvent::Resized;
    }
    if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == 0) {
        return TerminalEvent::None;  // mouse, focus, menu and key-up records
    }
    const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
    if (key.wVirtualKeyCode == VK_ESCAPE) {
        return TerminalEvent::Quit;
    }
    switch (key.uChar.UnicodeChar) {
        case L'q':
        case L'Q':
            return TerminalEvent::Quit;
        case L' ':
            return TerminalEvent::TogglePause;
        case L'l':
        case L'L':
            return TerminalEvent::ToggleLoop;
        default:
            return TerminalEvent::None;
    }
}

// ---- InterruptGuard ---------------------------------------------------------

struct InterruptGuard::State {};  // a State exists while the handler is installed

InterruptGuard::InterruptGuard(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
InterruptGuard::InterruptGuard(InterruptGuard&&) noexcept = default;

InterruptGuard::~InterruptGuard() {
    if (state_) {
        // Best effort: a destructor has nobody to report a failed removal to.
        static_cast<void>(SetConsoleCtrlHandler(on_ctrl, FALSE));
    }
}

Result<InterruptGuard> InterruptGuard::install() {
    auto state = std::make_unique<State>();
    g_stop.store(false);
    if (SetConsoleCtrlHandler(on_ctrl, TRUE) == 0) {
        return win_error("SetConsoleCtrlHandler");
    }
    return InterruptGuard(std::move(state));
}

bool InterruptGuard::requested() const noexcept { return g_stop.load(); }

}  // namespace vglyph::cli
