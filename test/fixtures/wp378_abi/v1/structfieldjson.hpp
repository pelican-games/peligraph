#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Pelican {

enum class StructFieldErrorCode : std::uint8_t {
    PayloadMustBeObject,
    UnknownField,
    MissingField,
    TypeMismatch,
    OutOfRange,
    InvalidEnum,
};

class StructFieldValidationError : public std::runtime_error {
    StructFieldErrorCode error_code;
    std::string field_path;

  public:
    StructFieldValidationError(StructFieldErrorCode code, std::string path,
                               std::string message)
        : std::runtime_error(std::move(message)), error_code(code),
          field_path(std::move(path)) {}

    StructFieldErrorCode code() const noexcept { return error_code; }
    std::string_view path() const noexcept { return field_path; }
};

} // namespace Pelican
