// windows.h must come after these defines and stays confined to this file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <string>

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

constexpr std::size_t kMaxChunk = std::size_t{1} << 20;

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

}  // namespace vglyph::cli
