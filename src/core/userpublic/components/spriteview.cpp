#include "spriteview.hpp"

#include "../../asset/atlasasset.hpp"
#include "../../ecs/enginecomponentregistrar.hpp"
#include "../../loader/componentcodec.hpp"
#include "../../loader/resolutionresult.hpp"
#include "predefined.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Pelican {
namespace {

bool finite(vec2 value) { return std::isfinite(value.x) && std::isfinite(value.y); }
bool finite(vec4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

} // namespace

// One acceptance implementation: the sprite_view codec. See the note on
// ColliderComponent::loadFromJsonArchive -- this is the ECS registerer's JSON
// loader, and it decodes through the codec instead of keeping a second parser
// with its own idea of which documents are legal.
void SpriteViewComponent::loadFromJsonArchive(const JsonArchiveLoader &archive) {
    const auto &json = *static_cast<const nlohmann::json *>(archive.ptr);
    const auto &codec = requireComponentCodec("sprite_view");
    auto decoded = codec.decodeAuthored(json);
    if (!decoded) throw ResolutionFailureException{std::move(decoded).error()};
    codec.applyRuntime(decoded.value(), this);
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
