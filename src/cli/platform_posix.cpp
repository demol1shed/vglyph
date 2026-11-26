#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include "platform.hpp"

namespace vglyph::cli {

struct ConsoleOutput::State {};

ConsoleOutput::ConsoleOutput() = default;
ConsoleOutput::~ConsoleOutput() = default;

namespace {

// write(2) may write less than asked or be interrupted by a signal.
[[nodiscard]] Status write_all(int fd, std::string_view bytes, const char* stream) {
    const char* p = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return make_error(ErrorCode::IoError,
                              std::string("write to ") + stream + " failed: " + std::strerror(errno));
        }
        if (n == 0) {
            return make_error(ErrorCode::IoError, std::string("write to ") + stream + " made no progress");
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return Status::success();
}

}  // namespace

Status write_stdout(std::string_view bytes) { return write_all(STDOUT_FILENO, bytes, "stdout"); }

Status write_stderr(std::string_view bytes) { return write_all(STDERR_FILENO, bytes, "stderr"); }

// ---- Terminal ---------------------------------------------------------------

namespace {

// Set by the signal handlers, which do nothing else.
volatile std::sig_atomic_t g_stop = 0;
volatile std::sig_atomic_t g_resized = 0;

void on_stop(int) { g_stop = 1; }
void on_winch(int) { g_resized = 1; }

// Alternate screen + hide cursor, and back. Leaving also ends a synchronized
// update that a Ctrl+C cut off (the tty drops queued output on a signal) and
// resets the colors.
constexpr std::string_view kEnterScreen = "\x1b[?1049h\x1b[?25l";
constexpr std::string_view kLeaveScreen = "\x1b[?2026l\x1b[0m\x1b[?25h\x1b[?1049l";
constexpr std::chrono::microseconds kMaxWait{100'000};

// The signals the player takes over. SIGTSTP is ignored: a suspended player
// would leave the terminal in alternate-screen mode.
struct Hook {
    int signal;
    void (*handler)(int);
    struct sigaction previous;
    bool installed;
};

[[nodiscard]] TerminalEvent pending_event() noexcept {
    if (g_stop != 0) {
        return TerminalEvent::Interrupted;
    }
    if (g_resized != 0) {
        g_resized = 0;
        return TerminalEvent::Resized;
    }
    return TerminalEvent::None;
}

}  // namespace

struct Terminal::State {
    termios saved{};
    bool raw = false;     // `saved` holds the original attributes to restore
    bool screen = false;  // the alternate screen was entered
    Hook hooks[4] = {{SIGINT, on_stop, {}, false},
                     {SIGTERM, on_stop, {}, false},
                     {SIGWINCH, on_winch, {}, false},
                     {SIGTSTP, SIG_IGN, {}, false}};
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
    if (s.raw) {
        static_cast<void>(tcsetattr(STDIN_FILENO, TCSAFLUSH, &s.saved));
    }
    for (Hook& hook : s.hooks) {
        if (hook.installed) {
            static_cast<void>(sigaction(hook.signal, &hook.previous, nullptr));
        }
    }
}

Result<Terminal> Terminal::open() {
    if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "play requires an interactive terminal (stdin and stdout must be a TTY)");
    }
    // From here on, returning early destroys `term`, which undoes what was set up.
    Terminal term(std::make_unique<State>());
    State& s = *term.state_;

    if (tcgetattr(STDIN_FILENO, &s.saved) != 0) {
        return make_error(ErrorCode::IoError, std::string("tcgetattr failed: ") + std::strerror(errno));
    }
    // Only line buffering and echo go: ISIG stays so Ctrl+C still signals, and
    // OPOST stays so "\n" still returns the carriage.
    termios raw = s.saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return make_error(ErrorCode::IoError, std::string("tcsetattr failed: ") + std::strerror(errno));
    }
    s.raw = true;

    g_stop = 0;
    g_resized = 0;
    for (Hook& hook : s.hooks) {
        struct sigaction action {};
        action.sa_handler = hook.handler;  // sa_flags 0: no SA_RESTART, so poll() sees EINTR
        sigemptyset(&action.sa_mask);
        if (sigaction(hook.signal, &action, &hook.previous) != 0) {
            return make_error(ErrorCode::IoError, std::string("sigaction failed: ") + std::strerror(errno));
        }
        hook.installed = true;
    }

    s.screen = true;
    if (const Status entered = write_stdout(kEnterScreen); !entered.ok()) {
        return entered.error();
    }
    return term;
}

Result<TerminalSize> Terminal::size() const {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0) {
        return make_error(ErrorCode::IoError, "cannot read the terminal size");
    }
    return TerminalSize{ws.ws_col, ws.ws_row};
}

TerminalEvent Terminal::wait(std::chrono::microseconds timeout) {
    if (const TerminalEvent event = pending_event(); event != TerminalEvent::None) {
        return event;
    }
    const auto bounded = std::clamp(timeout, std::chrono::microseconds::zero(), kMaxWait);
    const int ms = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(bounded).count());

    pollfd input{STDIN_FILENO, POLLIN, 0};
    const int ready = ::poll(&input, 1, ms);
    if (ready < 0 && errno != EINTR) {
        return TerminalEvent::Quit;
    }
    // A signal can interrupt the poll (EINTR) or land just before it.
    if (const TerminalEvent event = pending_event(); event != TerminalEvent::None) {
        return event;
    }
    if (ready <= 0) {
        return TerminalEvent::None;
    }

    char key = 0;
    const ssize_t n = ::read(STDIN_FILENO, &key, 1);
    if (n < 0) {
        return errno == EINTR ? TerminalEvent::None : TerminalEvent::Quit;
    }
    if (n == 0) {
        return TerminalEvent::Quit;  // the input was closed
    }
    switch (key) {
        case 'q':
        case 'Q':
        case '\x1b':
            return TerminalEvent::Quit;
        case ' ':
            return TerminalEvent::TogglePause;
        case 'l':
        case 'L':
            return TerminalEvent::ToggleLoop;
        default:
            return TerminalEvent::None;
    }
}

// ---- InterruptGuard ---------------------------------------------------------

struct InterruptGuard::State {
    Hook hooks[3] = {{SIGINT, on_stop, {}, false},
                     {SIGTERM, on_stop, {}, false},
                     {SIGHUP, on_stop, {}, false}};
};

InterruptGuard::InterruptGuard(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
InterruptGuard::InterruptGuard(InterruptGuard&&) noexcept = default;

InterruptGuard::~InterruptGuard() {
    if (!state_) {
        return;  // moved from
    }
    for (Hook& hook : state_->hooks) {
        if (hook.installed) {
            // Best effort: a destructor has nobody to report a failed restore to.
            static_cast<void>(sigaction(hook.signal, &hook.previous, nullptr));
        }
    }
}

Result<InterruptGuard> InterruptGuard::install() {
    // From here on, returning early destroys `guard`, which restores the handlers.
    InterruptGuard guard(std::make_unique<State>());
    g_stop = 0;
    for (Hook& hook : guard.state_->hooks) {
        if (hook.signal == SIGHUP) {
            struct sigaction current {};
            if (sigaction(SIGHUP, nullptr, &current) != 0) {
                return make_error(ErrorCode::IoError, std::string("sigaction failed: ") + std::strerror(errno));
            }
            if (current.sa_handler == SIG_IGN) {
                continue;  // nohup: the export outlives its terminal
            }
        }
        struct sigaction action {};
        action.sa_handler = hook.handler;
        action.sa_flags = SA_RESTART;  // file I/O carries on; the flag is polled per frame
        sigemptyset(&action.sa_mask);
        if (sigaction(hook.signal, &action, &hook.previous) != 0) {
            return make_error(ErrorCode::IoError, std::string("sigaction failed: ") + std::strerror(errno));
        }
        hook.installed = true;
    }
    return guard;
}

bool InterruptGuard::requested() const noexcept { return g_stop != 0; }

}  // namespace vglyph::cli
