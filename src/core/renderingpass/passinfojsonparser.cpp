#include "passinfojsonparser.hpp"
#include "debugdrawpassinfojsonparser.hpp"
#include "debugtextpassinfojsonparser.hpp"
#include "fullscreenpassinfojsonparser.hpp"
#include "genericrasterpassinfojsonparser.hpp"
#include "gizmopassinfojsonparser.hpp"
#include "passfieldownershipcapabilities.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>

namespace Pelican {

RenderPassType parsePassTypeFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json) {
    const auto pass_type = validatePassFieldOwnership(
        pass_json, buildPassFieldOwnershipCapabilities());
    pass_def.pass_info = makePassInfo(pass_type);
    pass_def.applyDefaultAttachmentOperations(pass_type);
    pass_def.resolution_domain =
        parseRenderResolutionDomain(
            pass_json, renderPassTypeName(pass_type), pass_def.name);
    return pass_type;
}

void parseFullscreenPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isFullscreen()) {
        return;
    }

    pass_def.fullscreenInfo() = parseFullscreenPassInfoFromJson(pass_json, pass_def.name);
}

void parseGenericRasterPassInfoIntoDefinition(
    PassDefinition &pass_def,
    const nlohmann::json &pass_json) {
    if (!pass_def.isGenericRaster()) {
        return;
    }
    pass_def.genericRasterInfo() =
        parseGenericRasterPassInfoFromJson(
            pass_json, pass_def.name,
            pass_def.output_color.size(),
            isConcreteRenderTarget(
                pass_def.output_depth));
}

void parseDebugDrawPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isDebugDraw()) {
        return;
    }

    pass_def.debugDrawInfo() = parseDebugDrawPassInfoFromJson(pass_json, pass_def.name);
}

void parseGizmoPassInfoIntoDefinition(PassDefinition &pass_def,
                                      const nlohmann::json &pass_json) {
    if (!pass_def.isGizmo()) return;
    pass_def.gizmoInfo() =
        parseGizmoPassInfoFromJson(pass_json, pass_def.name);
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

void parseVelocityPassInfoIntoDefinition(PassDefinition &pass_def,
                                         const nlohmann::json &pass_json) {
    if (!pass_def.isVelocity()) return;
    auto &info = pass_def.velocityInfo();
    info.vert_shader = makeShaderReference("engine://velocity", ShaderStage::vertex);
    info.skinned_vert_shader = makeShaderReference("engine://velocity_skinned", ShaderStage::vertex);
    info.frag_shader = makeShaderReference("engine://velocity", ShaderStage::fragment);
    if (!pass_json.contains("shader")) return;
    const auto &shader = pass_json.at("shader");
    if (!shader.is_object() || !shader.contains("vertex") || !shader.at("vertex").is_string() ||
        !shader.contains("fragment") || !shader.at("fragment").is_string()) {
        throw std::runtime_error("Velocity pass shader requires vertex and fragment strings: " +
                                 pass_def.name);
    }
    info.vert_shader = makeShaderReference(shader.at("vertex").get<std::string>(), ShaderStage::vertex);
    if (shader.contains("skinned_vertex")) {
        if (!shader.at("skinned_vertex").is_string()) {
            throw std::runtime_error("Velocity pass skinned_vertex shader must be a string: " + pass_def.name);
        }
        info.skinned_vert_shader = makeShaderReference(
            shader.at("skinned_vertex").get<std::string>(), ShaderStage::vertex);
    }
    info.frag_shader = makeShaderReference(shader.at("fragment").get<std::string>(), ShaderStage::fragment);
}

void parsePickingPassInfoIntoDefinition(PassDefinition &pass_def,
                                        const nlohmann::json &pass_json) {
    if (!pass_def.isPicking()) return;
    auto &info = pass_def.pickingInfo();
    info.vert_shader = makeShaderReference("engine://picking", ShaderStage::vertex);
    info.skinned_vert_shader =
        makeShaderReference("engine://picking_skinned", ShaderStage::vertex);
    info.frag_shader = makeShaderReference("engine://picking", ShaderStage::fragment);
    if (!pass_json.contains("shader")) return;
    const auto &shader = pass_json.at("shader");
    if (!shader.is_object() || !shader.contains("vertex") ||
        !shader.at("vertex").is_string() || !shader.contains("fragment") ||
        !shader.at("fragment").is_string()) {
        throw std::runtime_error(
            "Picking pass shader requires vertex and fragment strings: " +
            pass_def.name);
    }
    info.vert_shader = makeShaderReference(
        shader.at("vertex").get<std::string>(), ShaderStage::vertex);
    if (shader.contains("skinned_vertex")) {
        if (!shader.at("skinned_vertex").is_string()) {
            throw std::runtime_error(
                "Picking pass skinned_vertex shader must be a string: " +
                pass_def.name);
        }
        info.skinned_vert_shader = makeShaderReference(
            shader.at("skinned_vertex").get<std::string>(),
            ShaderStage::vertex);
    }
    info.frag_shader = makeShaderReference(
        shader.at("fragment").get<std::string>(), ShaderStage::fragment);
}

} // namespace Pelican
