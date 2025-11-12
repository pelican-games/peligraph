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
}

} // namespace Pelican
