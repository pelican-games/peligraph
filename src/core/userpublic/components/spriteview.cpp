#include "spriteview.hpp"

#include "../../asset/atlasasset.hpp"
#include "../../ecs/enginecomponentregistrar.hpp"
#include "../../loader/resolutionresult.hpp"
#include "predefined.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Pelican {
namespace {

void requireClosed(const nlohmann::json &json) {
    constexpr std::string_view known[]{"name", "texture", "size", "pivot", "color",
                                       "flip", "layer", "billboard"};
    if (!json.is_object())
        legacyReject("sprite_view.payload_not_object",
                     "sprite_view must be an object");
    for (const auto &[field, value] : json.items()) {
        (void)value;
        if (std::find(std::begin(known), std::end(known), field) == std::end(known))
            legacyReject("sprite_view.unknown_field",
                         "sprite_view has unknown field: " + field);
    }
}

vec2 readVec2(const nlohmann::json &json, const char *field, vec2 fallback) {
    const auto found = json.find(field);
    if (found == json.end()) return fallback;
    if (!found->is_array() || found->size() != 2)
        legacyReject("sprite_view.vec2_type",
                     std::string{"sprite_view field must be vec2: "} + field);
    if (!found->at(0).is_number() || !found->at(1).is_number())
        legacyReject("sprite_view.vec2_type",
                     std::string{"sprite_view field must be vec2: "} + field);
    return {found->at(0).get<float>(), found->at(1).get<float>()};
}

vec4 readVec4(const nlohmann::json &json, const char *field, vec4 fallback) {
    const auto found = json.find(field);
    if (found == json.end()) return fallback;
    if (!found->is_array() || found->size() != 4)
        legacyReject("sprite_view.vec4_type",
                     std::string{"sprite_view field must be vec4: "} + field);
    for (const auto &value : *found)
        if (!value.is_number())
            legacyReject("sprite_view.vec4_type",
                         std::string{"sprite_view field must be vec4: "} + field);
    return {found->at(0).get<float>(), found->at(1).get<float>(),
            found->at(2).get<float>(), found->at(3).get<float>()};
}

bool finite(vec2 value) { return std::isfinite(value.x) && std::isfinite(value.y); }
bool finite(vec4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

} // namespace

void SpriteViewComponent::loadFromJsonArchive(const JsonArchiveLoader &archive) {
    const auto &json = *static_cast<const nlohmann::json *>(archive.ptr);
    requireClosed(json);
    if (!json.contains("name") || !json.at("name").is_string() ||
        json.at("name").get_ref<const std::string &>() != "sprite_view")
        legacyReject("sprite_view.name_mismatch",
                     "sprite_view component name must be sprite_view");
    if (!json.contains("texture") || !json.at("texture").is_string())
        legacyReject("sprite_view.texture_type",
                     "sprite_view requires string texture");
    texture = json.at("texture").get<std::string>();

    has_explicit_size = json.contains("size") ? 1 : 0;
    size = readVec2(json, "size", {});
    pivot = readVec2(json, "pivot", {0.5f, 0.5f});
    color = readVec4(json, "color", {1.0f, 1.0f, 1.0f, 1.0f});
    if (const auto flip = json.find("flip"); flip != json.end()) {
        if (!flip->is_array() || flip->size() != 2 ||
            !flip->at(0).is_boolean() || !flip->at(1).is_boolean())
            legacyReject("sprite_view.flip_type",
                         "sprite_view flip must contain two booleans");
        flip_x = flip->at(0).get<bool>() ? 1 : 0;
        flip_y = flip->at(1).get<bool>() ? 1 : 0;
    } else {
        flip_x = flip_y = 0;
    }
    if (const auto layer_value = json.find("layer"); layer_value != json.end()) {
        if (!layer_value->is_number_integer())
            legacyReject("sprite_view.layer_type",
                         "sprite_view layer must be an int16");
        const auto loaded = layer_value->get<std::int64_t>();
        if (loaded < std::numeric_limits<std::int16_t>::min() ||
            loaded > std::numeric_limits<std::int16_t>::max())
            legacyReject("sprite_view.layer_range",
                         "sprite_view layer is outside int16");
        layer = static_cast<std::int16_t>(loaded);
    } else layer = 0;

    if (json.contains("billboard") && !json.at("billboard").is_string())
        legacyReject("sprite_view.billboard_type",
                     "sprite_view billboard must be none, y_axis, or full");
    const auto billboard_name =
        json.contains("billboard")
            ? json.at("billboard").get<std::string>()
            : std::string{"none"};
    if (billboard_name == "none") billboard = SpriteBillboard::none;
    else if (billboard_name == "y_axis") billboard = SpriteBillboard::y_axis;
    else if (billboard_name == "full") billboard = SpriteBillboard::full;
    else
        legacyReject("sprite_view.billboard_value",
                     "sprite_view billboard must be none, y_axis, or full");
    validate();
}

void SpriteViewComponent::validate() const {
    if (texture.empty())
        legacyReject("sprite_view.texture_empty",
                     "sprite_view texture must not be empty");
    (void)asset::parseSpriteAssetReference(texture);
    if (has_explicit_size && (!finite(size) || size.x <= 0.0f || size.y <= 0.0f))
        legacyReject("sprite_view.size_non_positive",
                     "sprite_view size must be finite and positive");
    if (!finite(pivot) || pivot.x < 0.0f || pivot.x > 1.0f ||
        pivot.y < 0.0f || pivot.y > 1.0f)
        legacyReject("sprite_view.pivot_out_of_range",
                     "sprite_view pivot must be finite and within 0..1");
    if (!finite(color))
        legacyReject("sprite_view.color_non_finite",
                     "sprite_view color must be finite");
}

void registerSpriteViewComponent() {
    internal::getEngineComponentRegisterer()
        .registerComponent<SpriteViewComponent>("sprite_view");
}

} // namespace Pelican
