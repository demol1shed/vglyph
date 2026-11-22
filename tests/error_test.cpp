#include <memory>
#include <string>

#include "test_framework.hpp"
#include "vglyph/error.hpp"

using vglyph::Error;
using vglyph::ErrorCode;
using vglyph::Result;
using vglyph::Status;

VGLYPH_TEST(result_holds_value) {
    const Result<int> r(42);
    VGLYPH_CHECK(r.ok());
    const int* value = r.value_if();
    VGLYPH_CHECK(value != nullptr);
    VGLYPH_CHECK(value != nullptr && *value == 42);
    VGLYPH_CHECK(r.error_if() == nullptr);
}

VGLYPH_TEST(result_holds_error) {
    const Result<int> r(vglyph::make_error(ErrorCode::LimitExceeded, "too big"));
    VGLYPH_CHECK(!r.ok());
    VGLYPH_CHECK(r.value_if() == nullptr);
    const Error* error = r.error_if();
    VGLYPH_CHECK(error != nullptr);
    VGLYPH_CHECK(error != nullptr && error->code == ErrorCode::LimitExceeded);
    VGLYPH_CHECK(error != nullptr && error->detail == "too big");
}

VGLYPH_TEST(result_supports_move_only_payload) {
    Result<std::unique_ptr<int>> r(std::make_unique<int>(7));
    VGLYPH_CHECK(r.ok());
    std::unique_ptr<int>* value = r.value_if();
    VGLYPH_CHECK(value != nullptr);
    if (value != nullptr) {
        std::unique_ptr<int> taken = std::move(*value);
        VGLYPH_CHECK(taken != nullptr && *taken == 7);
    }
}

VGLYPH_TEST(result_value_is_mutable_through_value_if) {
    Result<std::string> r(std::string("abc"));
    std::string* value = r.value_if();
    VGLYPH_CHECK(value != nullptr);
    if (value != nullptr) {
        value->push_back('d');
    }
    const std::string* after = r.value_if();
    VGLYPH_CHECK(after != nullptr && *after == "abcd");
}

VGLYPH_TEST(status_success_is_ok) {
    VGLYPH_CHECK(Status::success().ok());
    VGLYPH_CHECK(Status().ok());
    VGLYPH_CHECK(Status::success().error().code == ErrorCode::Ok);
}

VGLYPH_TEST(status_from_error_is_not_ok) {
    const Status s = vglyph::make_error(ErrorCode::InvalidArgument, "bad");
    VGLYPH_CHECK(!s.ok());
    VGLYPH_CHECK(s.error().code == ErrorCode::InvalidArgument);
    VGLYPH_CHECK(s.error().detail == "bad");
}

VGLYPH_TEST_MAIN()
