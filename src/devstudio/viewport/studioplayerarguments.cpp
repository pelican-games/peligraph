#include "studioplayerarguments.hpp"
#include "../../project/renderfeatureoverlay.hpp"

#include <string_view>

namespace PelicanStudio {
namespace {

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.starts_with(prefix);
}

} // namespace

std::vector<std::string> studioPlayerArgumentStrings(
    std::vector<std::string> configured,
    const std::string &project_root) {
    std::vector<std::string> additional;
    additional.reserve(configured.size());
    bool has_editor_transform_preset = false;
    for (std::size_t index = 0; index < configured.size(); ++index) {
        const auto &argument = configured[index];
        if (argument == "--rpc") {
            continue;
        }
        if (argument == "--project") {
            if (index + 1 < configured.size()) {
                ++index;
            }
            continue;
        }
        if (startsWith(argument, "--project=")) {
            continue;
        }
        if (argument == "--feature-overlay") {
            if (index + 1 < configured.size()) {
                ++index;
            }
            continue;
        }
        if (startsWith(argument, "--feature-overlay=")) {
            continue;
        }
        if (argument == "--editor-transform" ||
            startsWith(argument, "--editor-transform=")) {
            has_editor_transform_preset = true;
        }
        additional.push_back(argument);
    }

    std::vector<std::string> arguments{
        "--rpc",
        "--project",
        project_root,
        "--feature-overlay",
        std::string{Pelican::editorFeatureOverlayReference},
    };
    if (!has_editor_transform_preset) {
        arguments.emplace_back("--editor-transform");
    }
    arguments.insert(arguments.end(), additional.begin(), additional.end());
    return arguments;
}

} // namespace PelicanStudio
