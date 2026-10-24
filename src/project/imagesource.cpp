#include "imagesource.hpp"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace Pelican {

std::optional<ImageSourceDeclaration> parseImageSourceDeclaration(
    const nlohmann::json &target_json, const std::string &name) {
    const auto found = target_json.find("source");
    if (found == target_json.end()) return std::nullopt;
    if (!found->is_object() || !found->contains("asset") ||
        !found->at("asset").is_string() ||
        found->at("asset").get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            "Render target source requires a non-empty asset: " +
            name);
    }
    ImageSourceDeclaration source;
    source.asset = found->at("asset").get<std::string>();
    const auto choice = [&](const char *key, std::string &value,
                            std::initializer_list<std::string_view> allowed) {
        const auto field = found->find(key);
        if (field == found->end()) return;
        if (!field->is_string() ||
            std::find(allowed.begin(), allowed.end(),
                      field->get_ref<const std::string &>()) ==
                allowed.end()) {
            throw std::runtime_error(
                "Render target source " + std::string{key} +
                " is not one of its values: " + name);
        }
        value = field->get<std::string>();
    };
    choice("color_space", source.color_space, {"srgb", "linear"});
    choice("alpha", source.alpha, {"straight", "premultiply"});
    choice("mips", source.mips, {"none", "full"});
    if (!target_json.contains("width") || !target_json.contains("height")) {
        throw std::runtime_error(
            "Render target source requires the file's width and height: " +
            name);
    }
    if (target_json.value("history", false)) {
        throw std::runtime_error(
            "Render target source is read only and cannot keep history: " +
            name);
    }
    return source;
}

} // namespace Pelican
