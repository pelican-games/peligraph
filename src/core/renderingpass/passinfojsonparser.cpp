#include "passinfojsonparser.hpp"
#include "debugdrawpassinfojsonparser.hpp"
#include "debugtextpassinfojsonparser.hpp"
#include "fullscreenpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>

namespace Pelican {

void parsePassTypeFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    pass_def.pass_info = makePassInfo(parseStringField(pass_json, "type", "pass: " + pass_def.name));

    if (pass_def.isUi()
#if PELICAN_WITH_IMGUI
        || pass_def.isImGui()
#endif
    ) {
        pass_def.color_load_op = vk::AttachmentLoadOp::eLoad;
    }
}

void parseFullscreenPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isFullscreen()) {
        return;
    }

    pass_def.fullscreenInfo() = parseFullscreenPassInfoFromJson(pass_json, pass_def.name);
}

void parseDebugDrawPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isDebugDraw()) {
        return;
    }

    pass_def.debugDrawInfo() = parseDebugDrawPassInfoFromJson(pass_json, pass_def.name);
}

void parseDebugTextPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isDebugText()) {
        return;
    }

    pass_def.debugTextInfo() = parseDebugTextPassInfoFromJson(pass_json, pass_def.name);
}

void parseShadowDepthPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isShadowDepth()) {
        return;
    }

    auto &shadow_info = pass_def.shadowDepthInfo();
    shadow_info.vert_shader = makeShaderReference("engine://shadow_depth", ShaderStage::vertex);

    if (!pass_json.contains("shader")) {
        return;
    }
    const auto &shader = pass_json.at("shader");
    if (!shader.is_object()) {
        throw std::runtime_error("Shadow depth pass shader must be an object: " + pass_def.name);
    }
    if (!shader.contains("vertex") || !shader.at("vertex").is_string()) {
        throw std::runtime_error("Shadow depth pass shader requires vertex string: " + pass_def.name);
    }
    if (shader.contains("fragment")) {
        throw std::runtime_error("Shadow depth pass does not support fragment shader: " + pass_def.name);
    }
    shadow_info.vert_shader = makeShaderReference(shader.at("vertex").get<std::string>(), ShaderStage::vertex);
}

} // namespace Pelican
