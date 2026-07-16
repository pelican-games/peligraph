#include "openpbrmapping.hpp"

#include <nlohmann/json.hpp>

namespace Pelican {

std::optional<OpenPbrUnsupportedInputWarning>
makeOpenPbrUnsupportedInputWarning(const OpenPbrUnsupportedInputObservation &observation) {
    const bool authored_contribution = observation.authored && observation.authored_non_default;
    const bool connected_contribution =
        observation.connected && observation.connection_contributes;
    if (!authored_contribution && !connected_contribution) {
        return std::nullopt;
    }
    return OpenPbrUnsupportedInputWarning{
        std::string{openPbrUnsupportedInputWarningCode},
        observation.prim_path,
        observation.input,
        observation.fallback,
    };
}

std::string formatOpenPbrUnsupportedInputWarning(
    const OpenPbrUnsupportedInputWarning &warning) {
    nlohmann::ordered_json value{
        {"code", warning.code},
        {"prim_path", warning.prim_path},
        {"input", warning.input},
        {"fallback", warning.fallback},
    };
    return value.dump();
}

} // namespace Pelican
