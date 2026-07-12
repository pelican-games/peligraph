#pragma once

#include <geom/vec.hpp>
#include <serialize/jsonarchive.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

namespace Pelican {

enum class SpriteBillboard : std::uint8_t { none, y_axis, full };

// Scene v1 public component. A sprite is always an XY quad with +Z normal;
// floor placement and other planes are expressed only through Transform.
struct SpriteViewComponent {
    std::string texture;
    vec2 size{0.0f, 0.0f};
    std::uint8_t has_explicit_size = 0;
    vec2 pivot{0.5f, 0.5f};
    vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint8_t flip_x = 0;
    std::uint8_t flip_y = 0;
    std::int16_t layer = 0;
    SpriteBillboard billboard = SpriteBillboard::none;

    template <class T> void ref(T &archive) {
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, JsonArchiveLoader>) {
            loadFromJsonArchive(archive);
        } else {
            archive.prop("texture", texture);
            archive.prop("size", size);
            archive.prop("has_explicit_size", has_explicit_size);
            archive.prop("pivot", pivot);
            archive.prop("color", color);
            archive.prop("flip_x", flip_x);
            archive.prop("flip_y", flip_y);
            archive.prop("layer", layer);
            auto billboard_value = static_cast<std::uint8_t>(billboard);
            archive.prop("billboard", billboard_value);
        }
    }

    void loadFromJsonArchive(const JsonArchiveLoader &archive);
    void validate() const;
    void init() { validate(); }
};

void registerSpriteViewComponent();

} // namespace Pelican
