#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

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

}  // namespace vglyph::cli
