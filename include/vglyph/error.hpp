#pragma once

// Error model: every fallible function returns Result<T> or Status.
// No exceptions: values are read through get_if, never std::get.

#include <string>
#include <utility>
#include <variant>

namespace vglyph {

enum class ErrorCode {
    Ok,
    InvalidArgument,
    IoError,
    UnsupportedFormat,
    DecodeError,
    EncodeError,
    FontError,
    LimitExceeded,
    Cancelled,
    InternalError,
};

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string detail;
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string detail) {
    return Error{code, std::move(detail)};
}

// Holds either a T or an Error. Construct implicitly from either, so
// `return value;` and `return make_error(...);` both work.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(data_); }

    // Null when the result holds an error.
    [[nodiscard]] const T* value_if() const noexcept { return std::get_if<T>(&data_); }
    [[nodiscard]] T* value_if() noexcept { return std::get_if<T>(&data_); }

    // Null when the result holds a value.
    [[nodiscard]] const Error* error_if() const noexcept { return std::get_if<Error>(&data_); }

private:
    std::variant<T, Error> data_;
};

// Success or an Error, for functions with no value to return.
class [[nodiscard]] Status {
public:
    Status() = default;  // success
    Status(Error error) : error_(std::move(error)) {}

    [[nodiscard]] static Status success() { return Status(); }

    [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    Error error_;
};

}  // namespace vglyph
