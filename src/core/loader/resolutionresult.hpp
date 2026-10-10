#include "../userpublic/userdataerror.hpp"
#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Pelican {

// A user-authored value rejected by a resolution boundary. Paths are retained
// as tokens so RFC 6901 escaping happens exactly once, at serialization.
struct UserDataViolation {
    std::string code;
    std::vector<std::string> tokens;
    nlohmann::ordered_json fields = nlohmann::ordered_json::object();
    std::string leaf_message;

    friend bool operator==(const UserDataViolation &,
                           const UserDataViolation &) = default;
};

struct LegacyResolutionText {
    std::string legacy_branch_id;
    std::string message;

    friend bool operator==(const LegacyResolutionText &,
                           const LegacyResolutionText &) = default;
};

using StoredResolutionFailure =
    std::variant<UserDataViolation, LegacyResolutionText>;

// C++20 replacement for std::expected used at resolution boundaries. It is
// intentionally move-only: in particular, std::any cannot box the result and
// hide a missing unwrap from a downstream any_cast.
template <class T, class E = UserDataViolation>
class [[nodiscard]] ResolutionResult final {
    struct ValueTag {};
    struct ErrorTag {};

    std::variant<T, E> storage_;

    explicit ResolutionResult(ValueTag, T value)
        : storage_{std::in_place_index<0>, std::move(value)} {}
    explicit ResolutionResult(ErrorTag, E error)
        : storage_{std::in_place_index<1>, std::move(error)} {}

  public:
    static ResolutionResult success(T value) {
        return ResolutionResult{ValueTag{}, std::move(value)};
    }
    static ResolutionResult failure(E error) {
        return ResolutionResult{ErrorTag{}, std::move(error)};
    }

    ResolutionResult(const ResolutionResult &) = delete;
    ResolutionResult &operator=(const ResolutionResult &) = delete;
    ResolutionResult(ResolutionResult &&) noexcept(
        std::is_nothrow_move_constructible_v<std::variant<T, E>>) = default;
    ResolutionResult &operator=(ResolutionResult &&) noexcept(
        std::is_nothrow_move_assignable_v<std::variant<T, E>>) = default;
    ~ResolutionResult() = default;

    bool hasValue() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    T &value() & { return std::get<0>(storage_); }
    const T &value() const & { return std::get<0>(storage_); }
    T &&value() && { return std::get<0>(std::move(storage_)); }

    E &error() & { return std::get<1>(storage_); }
    const E &error() const & { return std::get<1>(storage_); }
    E &&error() && { return std::get<1>(std::move(storage_)); }
};

inline std::string resolutionJsonPointer(
    std::span<const std::string> tokens) {
    std::string result;
    for (const auto &token : tokens) {
        result.push_back('/');
        for (const char value : token) {
            if (value == '~') result += "~0";
            else if (value == '/') result += "~1";
            else result.push_back(value);
        }
    }
    return result;
}

inline UserDataViolation prependResolutionPath(
    UserDataViolation violation, std::span<const std::string> prefix) {
    std::vector<std::string> tokens;
    tokens.reserve(prefix.size() + violation.tokens.size());
    tokens.insert(tokens.end(), prefix.begin(), prefix.end());
    tokens.insert(tokens.end(),
                  std::make_move_iterator(violation.tokens.begin()),
                  std::make_move_iterator(violation.tokens.end()));
    violation.tokens = std::move(tokens);
    return violation;
}

inline nlohmann::ordered_json resolutionFailureJson(
    const StoredResolutionFailure &failure) {
    if (const auto *typed = std::get_if<UserDataViolation>(&failure)) {
        return nlohmann::ordered_json{
            {"code", typed->code},
            {"path", resolutionJsonPointer(typed->tokens)},
            {"fields", typed->fields},
            {"message", typed->leaf_message},
        };
    }
    const auto &legacy = std::get<LegacyResolutionText>(failure);
    return nlohmann::ordered_json{
        {"legacy_branch_id", legacy.legacy_branch_id},
        {"message", legacy.message},
    };
}

class LegacyRejectException final : public std::runtime_error {
    std::string legacy_branch_id_;

  public:
    LegacyRejectException(std::string legacy_branch_id, std::string message)
        : std::runtime_error{std::move(message)},
          legacy_branch_id_{std::move(legacy_branch_id)} {}

    const std::string &legacyBranchId() const noexcept {
        return legacy_branch_id_;
    }
};

[[noreturn]] inline void legacyReject(std::string_view legacy_branch_id,
                                      std::string message) {
    throw LegacyRejectException{std::string{legacy_branch_id},
                                std::move(message)};
}

struct LegacyBranchDescriptor {
    std::string_view legacy_branch_id;
    std::string_view producer;
};

// Production-owned and configuration-independent. In particular, disabling a
// runtime codec cannot remove its rejection descriptors from this inventory.
std::span<const LegacyBranchDescriptor> componentCodecLegacyBranches();

inline std::string resolutionFailureMessage(
    const StoredResolutionFailure &failure) {
    return std::visit(
        [](const auto &value) -> std::string {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Value, UserDataViolation>) {
                return value.leaf_message;
            } else {
                return value.message;
            }
        },
        failure);
}

// The transport thrown by runtime binders. It retains the typed alternative
// even when a binder adds the historical object context to what(). Deriving
// from runtime_error also preserves the pre-E-0a consumer catch contract.
class ResolutionFailureException final : public UserDataError {
    StoredResolutionFailure failure_;

  public:
    explicit ResolutionFailureException(StoredResolutionFailure failure)
        : UserDataError{resolutionFailureMessage(failure)},
          failure_{std::move(failure)} {}

    ResolutionFailureException(StoredResolutionFailure failure,
                               std::string contextual_message)
        : UserDataError{std::move(contextual_message)},
          failure_{std::move(failure)} {}

    const StoredResolutionFailure &failure() const noexcept {
        return failure_;
    }
};

inline std::string resolutionFloatText(float value) {
    char buffer[64]{};
    const auto converted = std::to_chars(
        std::begin(buffer), std::end(buffer), value,
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    if (converted.ec != std::errc{}) {
        throw std::logic_error("failed to format camera resolution value");
    }
    return std::string{buffer, converted.ptr};
}

// The sole camera cross-field rule. Callers supply only the path prefix owned
// by their boundary; the primary token is normatively znear.
inline std::optional<UserDataViolation> cameraCrossFieldViolation(
    float znear, float zfar,
    std::span<const std::string> path_prefix = {}) {
    if (zfar > znear) return std::nullopt;
    std::vector<std::string> tokens{path_prefix.begin(), path_prefix.end()};
    tokens.emplace_back("znear");
    return UserDataViolation{
        .code = "camera.zfar_not_greater_than_znear@1",
        .tokens = std::move(tokens),
        .fields = nlohmann::ordered_json{{"znear", znear}, {"zfar", zfar}},
        .leaf_message = "camera zfar (" + resolutionFloatText(zfar) +
                        ") must be greater than znear (" +
                        resolutionFloatText(znear) + ")",
    };
}

} // namespace Pelican
