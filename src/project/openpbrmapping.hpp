#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Pelican {

inline constexpr std::string_view openPbrUnsupportedInputWarningCode =
    "OPENPBR_UNSUPPORTED_INPUT";

// Observation boundary shared with the future U-USD0c importer. Merely seeing
// an unsupported node/input is intentionally insufficient to produce a WARN.
struct OpenPbrUnsupportedInputObservation {
    std::string prim_path;
    std::string input;
    std::string fallback;
    bool authored = false;
    bool authored_non_default = false;
    bool connected = false;
    bool connection_contributes = false;
};

struct OpenPbrUnsupportedInputWarning {
    std::string code;
    std::string prim_path;
    std::string input;
    std::string fallback;
};

std::optional<OpenPbrUnsupportedInputWarning>
makeOpenPbrUnsupportedInputWarning(const OpenPbrUnsupportedInputObservation &observation);

// Stable compact JSON surface for logs, import reports, and machine fixtures.
std::string formatOpenPbrUnsupportedInputWarning(
    const OpenPbrUnsupportedInputWarning &warning);

} // namespace Pelican
