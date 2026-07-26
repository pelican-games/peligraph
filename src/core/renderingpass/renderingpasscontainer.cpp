#include "renderingpasscontainer.hpp"
#include "framegraphruntime.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

template <typename Visitor>
void visitPublishedRenderingPasses(
    const RenderPipelineRuntimeGeneration &generation,
    Visitor &&visitor) {
    for (const auto rendering_pass_id :
         generation.rendering_pass_ids) {
        const auto *program =
            generation.find(rendering_pass_id);
        if (program == nullptr) {
            throw std::logic_error(
                "Published render pipeline pass table is inconsistent");
        }
        visitor(program->rendering_pass);
    }
}

} // namespace

RenderingPassContainer::RenderingPassContainer()
    : runtime_publication{nullptr} {}

RenderingPassContainer::~RenderingPassContainer() {}

RenderingPassId RenderingPassContainer::registerCompiledRenderingPass(CompiledRenderingPass pass) {
    if (auto it = name_to_id.find(pass.name); it != name_to_id.end()) {
        return it->second;
    }

    const auto pass_name = pass.name;
    auto id = rendering_passes.reg(std::move(pass));
    name_to_id.emplace(pass_name, id);
    registered_pass_ids.push_back(id);
    return id;
}

RenderingPassId RenderingPassContainer::getRenderingPassIdByName(const std::string &name) const {
    const auto generation = snapshot();
    if (generation != nullptr) {
        const auto found = generation->name_to_id.find(name);
        return found != generation->name_to_id.end()
                   ? found->second
                   : invalidRenderingPassId();
    }
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return invalidRenderingPassId();
}

const CompiledRenderingPass &RenderingPassContainer::getCompiledRenderingPass(RenderingPassId rendering_pass_id) const {
    const auto generation = snapshot();
    if (generation != nullptr) {
        const auto *program =
            generation->find(rendering_pass_id);
        if (program == nullptr) {
            throw std::out_of_range(
                "Published rendering pass not found");
        }
        return program->rendering_pass;
    }
    return rendering_passes.get(rendering_pass_id);
}

const std::vector<RenderingPassId> &RenderingPassContainer::getRegisteredPassIds() const {
    const auto generation = snapshot();
    if (generation != nullptr) {
        return generation->rendering_pass_ids;
    }
    return registered_pass_ids;
}

void RenderingPassContainer::setEnabledFeatures(std::vector<std::string> feature_names) {
    enabled_feature_names = std::move(feature_names);
}

bool RenderingPassContainer::isFeatureEnabled(std::string_view feature_name) const {
    if (const auto generation = snapshot()) {
        return std::find(
                   generation->enabled_feature_names.begin(),
                   generation->enabled_feature_names.end(),
                   feature_name) !=
               generation->enabled_feature_names.end();
    }
    const auto &features = getEnabledFeatures();
    return std::find(features.begin(), features.end(),
                     feature_name) != features.end();
}

const std::vector<std::string> &RenderingPassContainer::getEnabledFeatures() const {
    const auto generation = snapshot();
    if (generation != nullptr) {
        return generation->enabled_feature_names;
    }
    return enabled_feature_names;
}

void RenderingPassContainer::bindRuntimePublication(
    std::shared_ptr<const RenderPipelineRuntimePublication>
        publication) noexcept {
    runtime_publication.store(
        std::move(publication), std::memory_order_release);
}

std::shared_ptr<const RenderPipelineRuntimeGeneration>
RenderingPassContainer::snapshot() const noexcept {
    const auto publication =
        runtime_publication.load(std::memory_order_acquire);
    return publication != nullptr
               ? publication->active_generation.load(
                     std::memory_order_acquire)
               : nullptr;
}

std::uint64_t
RenderingPassContainer::activeGeneration() const noexcept {
    const auto generation = snapshot();
    return generation != nullptr ? generation->generation : 0;
}

bool RenderingPassContainer::hasMaterialPasses() const {
    if (const auto generation = snapshot()) {
        bool found = false;
        visitPublishedRenderingPasses(
            *generation,
            [&found](const CompiledRenderingPass &rendering_pass) {
                found = found ||
                        std::any_of(
                            rendering_pass.passes.begin(),
                            rendering_pass.passes.end(),
                            [](const auto &pass) {
                                return pass.definition.isMaterial();
                            });
            });
        return found;
    }
    for (const auto rendering_pass_id :
         registered_pass_ids) {
        const auto &rendering_pass =
            rendering_passes.get(rendering_pass_id);
        if (std::any_of(rendering_pass.passes.begin(), rendering_pass.passes.end(),
                        [](const auto &pass) { return pass.definition.isMaterial(); })) {
            return true;
        }
    }
    return false;
}

bool RenderingPassContainer::supportsMaterialPass(
    MaterialRouteClass route, MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass) const {
    return !materialPassRenderingBindings(
                 route, shader_contract, exact_pass)
                 .empty();
}

std::vector<MaterialPassRenderingBinding>
RenderingPassContainer::materialPassRenderingBindings(
    MaterialRouteClass route,
    MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass) const {
    std::vector<MaterialPassRenderingBinding> result;
    const auto collect =
        [&](const CompiledRenderingPass &rendering_pass) {
            for (const auto &compiled :
                 rendering_pass.passes) {
                const auto &pass = compiled.definition;
                if (!pass.isMaterial() ||
                    (exact_pass &&
                     pass.name != *exact_pass) ||
                    !materialPassAcceptsMaterial(
                        pass.materialInfo().contract,
                        pass.name, route,
                        shader_contract,
                        exact_pass)) {
                    continue;
                }
                result.push_back(
                    MaterialPassRenderingBinding{
                        .pass_name = pass.name,
                        .rasterization_samples =
                            pass.rasterization_samples,
                        .rendering =
                            compiled.rendering,
                    });
            }
        };
    if (const auto generation = snapshot()) {
        visitPublishedRenderingPasses(
            *generation, collect);
    } else {
        for (const auto rendering_pass_id :
             registered_pass_ids) {
            collect(rendering_passes.get(
                rendering_pass_id));
        }
    }
    return result;
}

vk::SampleCountFlagBits
RenderingPassContainer::materialRasterizationSamples(
    MaterialShaderContract shader_contract) const {
    std::optional<vk::SampleCountFlagBits> samples;
    const auto collect =
        [&](const CompiledRenderingPass &rendering_pass) {
            for (const auto &compiled :
                 rendering_pass.passes) {
                const auto &pass = compiled.definition;
                if (!pass.isMaterial()) continue;
                const auto pass_shader_contract =
                    materialPassShaderContract(
                        pass.materialInfo().contract);
                const bool legacy_compatible =
                    pass.materialInfo().contract ==
                        MaterialPassContract::legacy_gbuffer_v1 &&
                    (shader_contract ==
                         MaterialShaderContract::legacy_gbuffer_v1 ||
                     shader_contract ==
                         MaterialShaderContract::gbuffer_v1);
                if (pass_shader_contract != shader_contract &&
                    !legacy_compatible) {
                    continue;
                }
                if (samples &&
                    *samples != pass.rasterization_samples) {
                    throw std::runtime_error(
                        "material shader contract is bound to passes with "
                        "different rasterization sample counts: " +
                        std::string{
                            materialShaderContractName(
                                shader_contract)});
                }
                samples = pass.rasterization_samples;
            }
        };
    if (const auto generation = snapshot()) {
        visitPublishedRenderingPasses(*generation, collect);
    } else {
        for (const auto rendering_pass_id :
             registered_pass_ids) {
            collect(rendering_passes.get(rendering_pass_id));
        }
    }
    return samples.value_or(vk::SampleCountFlagBits::e1);
}

} // namespace Pelican
