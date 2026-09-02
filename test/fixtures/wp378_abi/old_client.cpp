#include "v1/structfieldjson.hpp"

#if defined(_WIN32)
#define WP378_ABI_EXPORT extern "C" __declspec(dllexport)
#else
#define WP378_ABI_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

void throwStructFieldError() {
    throw Pelican::StructFieldValidationError{
        Pelican::StructFieldErrorCode::OutOfRange, "camera.znear",
        "field 'camera.znear' must be greater than zero"};
}

} // namespace

using Wp378ThrowStructFieldError = void (*)();

WP378_ABI_EXPORT Wp378ThrowStructFieldError
pelican_wp378_struct_field_error_thrower() {
    return &throwStructFieldError;
}
