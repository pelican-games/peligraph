#include "standardmaterialresource.hpp"
#include "battery/embed.hpp"
#include "materialcontainer.hpp"

namespace Pelican {

StandardMaterialResource::StandardMaterialResource(DependencyContainer &con) {
    auto &mat_con = con.get<MaterialContainer>();

    const auto vert_shader = b::embed<"default.vert.spv">();
    std_vert = mat_con.registerShader(vert_shader.length(), vert_shader.data());

    const auto frag_shader = b::embed<"default.frag.spv">();
    std_frag = mat_con.registerShader(frag_shader.length(), frag_shader.data());

    uint8_t texdata_transparent[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_transparent[i * 4 + 0] = 0;
        texdata_transparent[i * 4 + 1] = 0;
        texdata_transparent[i * 4 + 2] = 0;
        texdata_transparent[i * 4 + 3] = 0;
    }
    tex_transparent = mat_con.registerTexture(vk::Extent3D(4, 4, 1), texdata_transparent);

    uint8_t texdata_white[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_white[i * 4 + 0] = 255;
        texdata_white[i * 4 + 1] = 255;
        texdata_white[i * 4 + 2] = 255;
        texdata_white[i * 4 + 3] = 255;
    }
    tex_white = mat_con.registerTexture(vk::Extent3D(4, 4, 1), texdata_white);

    uint8_t texdata_black[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_black[i * 4 + 0] = 0;
        texdata_black[i * 4 + 1] = 0;
        texdata_black[i * 4 + 2] = 0;
        texdata_black[i * 4 + 3] = 255;
    }
    tex_black = mat_con.registerTexture(vk::Extent3D(4, 4, 1), texdata_black);
}

} // namespace Pelican
