#include "shaderresolution.hpp"

#include "../loader/pathresolver.hpp"
#include "../renderingpass/passimplementationregistry.hpp"
#include "../renderingpass/renderingpass.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

struct EffectiveShaderStage {
    DeclaredShaderStage stage = DeclaredShaderStage::vertex;
    std::optional<std::size_t> index;
    std::string_view effective_ref;
    ShaderSourceStage source_stage = ShaderSourceStage::vertex;
};

std::string_view wireStageName(DeclaredShaderStage stage) {
    switch (stage) {
    case DeclaredShaderStage::vertex:
        return "vertex";
    case DeclaredShaderStage::skinned_vertex:
        return "skinned_vertex";
    case DeclaredShaderStage::fragment:
        return "fragment";
    case DeclaredShaderStage::compute:
        return "compute";
    case DeclaredShaderStage::raygen:
        return "raygen";
    case DeclaredShaderStage::miss:
        return "miss";
    case DeclaredShaderStage::closesthit:
        return "closesthit";
    }
    throw std::runtime_error("unknown declared shader stage");
}

const DeclaredShaderReference *findDeclared(
    const std::vector<DeclaredShaderReference> &declared,
    DeclaredShaderStage stage,
    std::optional<std::size_t> index) {
    const auto found = std::find_if(
        declared.begin(), declared.end(),
        [&](const DeclaredShaderReference &candidate) {
            return candidate.stage == stage &&
                   candidate.index == index;
        });
    return found == declared.end() ? nullptr : &*found;
}

bool identityFullscreenProvider(const PassDefinition &pass) {
    if (!pass.implementation_selection) {
        return true;
    }
    const auto &selection = *pass.implementation_selection;
    return selection.provider ==
               builtinFullscreenPassImplementationProvider &&
           selection.provider_owner ==
               internal::engineRegistrationOwner;
}

Json stageToJson(
    const EffectiveShaderStage &stage,
    const std::vector<DeclaredShaderReference> &declared,
    std::string_view origin,
    const PathResolver &path_resolver) {
    Json result{
        {"stage", wireStageName(stage.stage)},
        {"effective_ref", stage.effective_ref},
        {"origin", origin},
    };
    if (stage.index) {
        result["index"] = *stage.index;
    }
    if (const auto *authored =
            findDeclared(declared, stage.stage, stage.index)) {
        result["declared_ref"] = authored->ref;
    }

    const auto source =
        path_resolver.resolveShaderSourceOpenReference(
            stage.effective_ref, stage.source_stage);
    if (source.openable()) {
        result["source_open_ref"] = *source.logical_ref;
    } else {
        if (!source.reason) {
            throw std::logic_error(
                "shader source resolution is neither openable nor named");
        }
        result["source_open_ref"] = nullptr;
        result["source_open_reason"] =
            shaderSourceOpenReasonName(*source.reason);
    }
    return result;
}

Json resolvedStages(
    std::vector<EffectiveShaderStage> stages,
    const std::vector<DeclaredShaderReference> &declared,
    bool provider_pair, bool declaration_parsed,
    const PathResolver &path_resolver) {
    Json encoded = Json::array();
    for (const auto &stage : stages) {
        const auto *authored =
            findDeclared(declared, stage.stage, stage.index);
        const std::string_view origin =
            provider_pair
                ? "provider"
                : authored != nullptr ? "authored"
                  : declaration_parsed ? "engine_default"
                                       : "generated";
        encoded.push_back(stageToJson(
            stage, declared, origin, path_resolver));
    }
    return Json{
        {"state", "resolved"},
        {"stages", std::move(encoded)},
    };
}

Json engineFixedResolution(
    std::string_view stem) {
    Json stages = Json::array();
    for (const auto stage : {"vertex", "fragment"}) {
        stages.push_back(Json{
            {"stage", stage},
            {"effective_ref", stem},
            {"origin", "generated"},
            {"source_open_ref", nullptr},
            {"source_open_reason", "embedded_engine_resource"},
        });
    }
    return Json{
        {"state", "resolved"},
        {"stages", std::move(stages)},
    };
}

Json passShaderResolution(
    const PassDefinition &pass,
    const PathResolver &path_resolver) {
    if (pass.isMaterial()) {
        return Json{{"state", "material_owned"}};
    }
    if (pass.isUi()) {
        return engineFixedResolution("engine://ui");
    }
#if PELICAN_WITH_IMGUI
    if (pass.isImGui()) {
        return engineFixedResolution("engine://imgui");
    }
#endif

    std::vector<EffectiveShaderStage> stages;
    bool provider_pair = false;
    if (pass.isFullscreen()) {
        const auto &info = pass.fullscreenInfo();
        stages = {
            {DeclaredShaderStage::vertex, std::nullopt,
             info.vert_shader.ref, ShaderSourceStage::vertex},
            {DeclaredShaderStage::fragment, std::nullopt,
             info.frag_shader.ref, ShaderSourceStage::fragment},
        };
        provider_pair = !identityFullscreenProvider(pass);
    } else if (pass.isGenericRaster()) {
        const auto &info = pass.genericRasterInfo();
        stages.push_back(
            {DeclaredShaderStage::vertex, std::nullopt,
             info.vert_shader.ref, ShaderSourceStage::vertex});
        if (info.frag_shader) {
            stages.push_back(
                {DeclaredShaderStage::fragment, std::nullopt,
                 info.frag_shader->ref,
                 ShaderSourceStage::fragment});
        }
    } else if (pass.isDebugDraw()) {
        const auto &info = pass.debugDrawInfo();
        stages = {
            {DeclaredShaderStage::vertex, std::nullopt,
             info.vert_shader.ref, ShaderSourceStage::vertex},
            {DeclaredShaderStage::fragment, std::nullopt,
             info.frag_shader.ref, ShaderSourceStage::fragment},
        };
    } else if (pass.isGizmo()) {
        const auto &info = pass.gizmoInfo();
        stages = {
            {DeclaredShaderStage::vertex, std::nullopt,
             info.vert_shader.ref, ShaderSourceStage::vertex},
            {DeclaredShaderStage::fragment, std::nullopt,
             info.frag_shader.ref, ShaderSourceStage::fragment},
        };
    } else if (pass.isDebugText()) {
        const auto &info = pass.debugTextInfo();
        stages = {
            {DeclaredShaderStage::vertex, std::nullopt,
             info.vert_shader.ref, ShaderSourceStage::vertex},
            {DeclaredShaderStage::fragment, std::nullopt,
             info.frag_shader.ref, ShaderSourceStage::fragment},
        };
    } else if (pass.isShadowDepth()) {
        const auto &info = pass.shadowDepthInfo();
        stages = {
            {DeclaredShaderStage::vertex, std::nullopt,
             info.vert_shader.ref, ShaderSourceStage::vertex},
        };
    } else if (pass.isVelocity()) {
        const auto &info = pass.velocityInfo();
        stages = {
            {DeclaredShaderStage::vertex, std::nullopt,
             info.vert_shader.ref, ShaderSourceStage::vertex},
            {DeclaredShaderStage::skinned_vertex, std::nullopt,
             info.skinned_vert_shader.ref,
             ShaderSourceStage::vertex},
            {DeclaredShaderStage::fragment, std::nullopt,
             info.frag_shader.ref, ShaderSourceStage::fragment},
        };
    } else if (pass.isPicking()) {
        const auto &info = pass.pickingInfo();
        stages = {
            {DeclaredShaderStage::vertex, std::nullopt,
             info.vert_shader.ref, ShaderSourceStage::vertex},
            {DeclaredShaderStage::skinned_vertex, std::nullopt,
             info.skinned_vert_shader.ref,
             ShaderSourceStage::vertex},
            {DeclaredShaderStage::fragment, std::nullopt,
             info.frag_shader.ref, ShaderSourceStage::fragment},
        };
    } else {
        throw std::runtime_error(
            "runtime pass has no WP354 shader family: " +
            pass.name);
    }
    return resolvedStages(
        std::move(stages), pass.declared_shader_refs,
        provider_pair, pass.shader_declaration_parsed,
        path_resolver);
}

Json computeShaderResolution(
    const ComputeTaskDefinition &task,
    const PathResolver &path_resolver) {
    std::vector<EffectiveShaderStage> stages;
    if (!task.ray_tracing) {
        stages.push_back(
            {DeclaredShaderStage::compute, std::nullopt,
             task.shader.ref, ShaderSourceStage::compute});
    } else {
        const auto &ray = *task.ray_tracing;
        stages.push_back(
            {DeclaredShaderStage::raygen, std::nullopt,
             ray.raygen.ref, ShaderSourceStage::raygen});
        for (std::size_t index = 0; index < ray.misses.size(); ++index) {
            stages.push_back(
                {DeclaredShaderStage::miss, index,
                 ray.misses[index].ref,
                 ShaderSourceStage::miss});
        }
        for (std::size_t index = 0;
             index < ray.closest_hits.size(); ++index) {
            stages.push_back(
                {DeclaredShaderStage::closesthit, index,
                 ray.closest_hits[index].ref,
                 ShaderSourceStage::closesthit});
        }
    }
    return resolvedStages(
        std::move(stages), task.declared_shader_refs,
        false, task.shader_declaration_parsed,
        path_resolver);
}

} // namespace

std::span<const ShaderResolutionFamilyContract>
shaderResolutionFamilyContracts() {
    static constexpr std::array<std::string_view, 0> no_stages{};
    static constexpr std::string_view vertex_fragment[] = {
        "vertex", "fragment"};
    static constexpr std::string_view vertex_optional_fragment[] = {
        "vertex", "fragment?"};
    static constexpr std::string_view vertex[] = {"vertex"};
    static constexpr std::string_view skinned[] = {
        "vertex", "skinned_vertex", "fragment"};
    static constexpr std::string_view compute_or_ray[] = {
        "compute", "raygen", "miss[index]*", "closesthit[index]*"};
    static constexpr ShaderResolutionFamilyContract contracts[] = {
        {"material", "material_owned", "material_owned", no_stages},
        {"fullscreen", "resolved", "path_owned", vertex_fragment},
        {"raster", "resolved", "path_owned", vertex_optional_fragment},
        {"output_transform", "resolved", "path_owned", vertex_fragment},
        {"debug_draw", "resolved", "path_owned", vertex_fragment},
        {"gizmo", "resolved", "path_owned", vertex_fragment},
        {"debug_text", "resolved", "path_owned", vertex_fragment},
        {"shadow_depth", "resolved", "path_owned", vertex},
        {"velocity", "resolved", "path_owned", skinned},
        {"picking", "resolved", "path_owned", skinned},
        {"ui", "resolved", "engine_fixed", vertex_fragment},
        {"imgui", "resolved", "engine_fixed", vertex_fragment},
        {"canonical_anchor", "not_applicable", "none", no_stages},
        {"snapshot_copy", "not_applicable", "none", no_stages},
        {"compute_task", "resolved", "path_owned", compute_or_ray},
    };
    return contracts;
}

void appendShaderResolution(
    nlohmann::json &frame_plan,
    const CompiledRenderingPass &rendering_pass,
    const PathResolver &path_resolver) {
    if (!frame_plan.is_object() ||
        frame_plan.value("schema", std::string{}) !=
            "pelican.frame_plan" ||
        !frame_plan.contains("nodes") ||
        !frame_plan.at("nodes").is_array()) {
        throw std::invalid_argument(
            "appendShaderResolution requires a pelican.frame_plan node array");
    }

    std::map<std::string, const PassDefinition *, std::less<>> passes;
    for (const auto &compiled : rendering_pass.passes) {
        if (!passes.emplace(
                 compiled.definition.name,
                 &compiled.definition)
                 .second) {
            throw std::runtime_error(
                "duplicate runtime pass while projecting shader resolution: " +
                compiled.definition.name);
        }
    }
    std::map<std::string, const ComputeTaskDefinition *, std::less<>> tasks;
    for (const auto &compiled : rendering_pass.compute_tasks) {
        if (!tasks.emplace(
                 compiled.definition.name,
                 &compiled.definition)
                 .second) {
            throw std::runtime_error(
                "duplicate runtime compute task while projecting shader resolution: " +
                compiled.definition.name);
        }
    }

    frame_plan["profile"] = "runtime";
    for (auto &node : frame_plan["nodes"]) {
        if (!node.is_object() || !node.contains("name") ||
            !node.at("name").is_string() ||
            !node.contains("kind") ||
            !node.at("kind").is_string()) {
            throw std::runtime_error(
                "runtime frame-plan node has invalid identity while projecting shader resolution");
        }
        if (node.contains("shader_resolution")) {
            throw std::runtime_error(
                "runtime frame-plan node already has shader_resolution: " +
                node.at("name").get<std::string>());
        }
        const auto name = node.at("name").get<std::string>();
        const auto kind = node.at("kind").get<std::string>();
        if (kind == "anchor" || kind == "snapshot_copy") {
            node["shader_resolution"] =
                Json{{"state", "not_applicable"}};
            continue;
        }
        if (const auto pass = passes.find(name);
            pass != passes.end()) {
            node["shader_resolution"] =
                passShaderResolution(
                    *pass->second, path_resolver);
            continue;
        }
        if (const auto task = tasks.find(name);
            task != tasks.end()) {
            node["shader_resolution"] =
                computeShaderResolution(
                    *task->second, path_resolver);
            continue;
        }
        throw std::runtime_error(
            "runtime frame-plan node has no compiled shader owner: " +
            name + " (" + kind + ")");
    }
}

} // namespace Pelican
