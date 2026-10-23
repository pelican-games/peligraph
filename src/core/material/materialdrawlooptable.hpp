#pragma once

#include "../../project/materialdrawloop.hpp"
#include "../resourcecontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/shaderlibrary.hpp"
#include "material.hpp"
#include "materialcontainer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

enum class VertexMode;

// One resolved step: the material record that supplies the descriptor set,
// the GPU material index and the screen inputs, and the pipeline to draw
// with (nullopt: that material's own pipeline).
struct ResolvedMaterialDrawStep {
    GlobalMaterialId material;
    std::optional<PipelineHandle> pipeline;

    bool operator==(const ResolvedMaterialDrawStep &) const = default;
};

// Resolve one mode's steps against the base material's shared variant map.
// Pipeline allocation is needed only for an explicit override step.
template <typename VariantResource, typename OverridePipeline>
std::vector<ResolvedMaterialDrawStep> resolveMaterialDrawSteps(
    GlobalMaterialId material, const MaterialDrawSubject &subject,
    const MaterialDrawStepList &steps, std::string_view node,
    bool runtime_compiler_available, VariantResource variant_resource,
    OverridePipeline override_pipeline) {
    std::vector<ResolvedMaterialDrawStep> resolved;
    resolved.reserve(steps.size());
    for (const auto &step : steps) {
        validateMaterialDrawStep(step, subject, node, runtime_compiler_available);
        switch (step.kind) {
        case MaterialDrawStepKind::material:
            resolved.push_back({material, std::nullopt});
            break;
        case MaterialDrawStepKind::variant:
            resolved.push_back({variant_resource(step.variant), std::nullopt});
            break;
        case MaterialDrawStepKind::override_pipeline:
            resolved.push_back({material, override_pipeline(step)});
            break;
        }
    }
    return resolved;
}

// LP-2: the draw-step table of one renderer runtime generation, keyed by
// (material pass name, base material). It owns the override pipelines (and,
// for defines, the shader bundles) it built: they are never shared with the
// material pipeline cache, and they are retired with the table, i.e. with
// the generation that holds it.
//
// Every live table is registered, so a material registered while
// generations are alive (glTF load, spawn) is evaluated by all of them: the
// published one, a candidate being prepared, and older ones still retained
// by frames in flight.
class MaterialDrawLoopTable {
  public:
    explicit MaterialDrawLoopTable(std::uint64_t generation);
    ~MaterialDrawLoopTable();
    MaterialDrawLoopTable(const MaterialDrawLoopTable &) = delete;
    MaterialDrawLoopTable &operator=(const MaterialDrawLoopTable &) = delete;

    // Replaces the rules and evaluates them for every registered material.
    // Throws the named refusal of the first material that cannot be served;
    // the table is then unchanged and every pipeline built on the way is
    // released. On success the previous overrides are retired.
    void install(std::shared_ptr<const MaterialDrawLoopRules> rules);
    // Evaluates the rules for one material (registered, or its variants
    // changed). Same guarantee as install().
    void evaluateMaterial(GlobalMaterialId material);
    // Drops a released material's entries. Its overrides stay until the
    // table is retired (they may be shared with other materials).
    void forgetMaterial(GlobalMaterialId material) noexcept;

    // nullptr: the pass has no entry for this material and draws it the
    // default way. An empty list draws nothing.
    const std::vector<ResolvedMaterialDrawStep> *find(
        std::string_view pass, GlobalMaterialId material, VertexMode mode = VertexMode::standard) const;
    bool empty() const noexcept { return passes_.empty(); }
    std::uint64_t generation() const noexcept { return generation_; }
    const std::shared_ptr<const MaterialDrawLoopRules> &rules() const noexcept {
        return rules_;
    }
    std::size_t overridePipelineCount() const noexcept {
        return overrides_.size();
    }
    std::size_t defineCompileCount() const noexcept {
        return compiled_.size();
    }

    // One override pipeline and what it was derived from, so a render
    // generation reload that rebuilds the base pipeline in place (another
    // resource interface, ray-query frame set or pass-input defines) can
    // rebuild the override from the base's candidate description too.
    struct OverridePipeline {
        PipelineHandle pipeline;
        PipelineHandle base_pipeline;
        // A material that used the base when the override was built.
        GlobalMaterialId base_material;
        MaterialDrawRenderStateOverride render_state;
        // Set when the override compiled defines: its own shader pair.
        std::optional<SurfaceShaderBundleIds> define_bundles;
    };
    void forEachOverride(
        const std::function<void(const OverridePipeline &)> &visit) const;

    // Every table alive right now, oldest first.
    static void forEachLive(
        const std::function<void(MaterialDrawLoopTable &)> &visit);
    // Registration hooks called by MaterialContainer. Each table evaluates
    // or forgets the material. The first refusal is rethrown as is; the
    // caller undoes the registration (and so forgets the material, or
    // evaluates the base again without the refused variants).
    static void materialRegisteredInLiveTables(GlobalMaterialId material);
    static void materialReleasedFromLiveTables(
        GlobalMaterialId material) noexcept;

  private:
    struct CompiledDefines {
        SurfaceShaderBundleIds bundles;
    };
    using Entries = std::map<std::pair<int, VertexMode>,
                             std::vector<ResolvedMaterialDrawStep>>;
    // What a build adds, so a refusal can release exactly that.
    struct Additions {
        std::vector<std::string> override_keys;
        std::vector<std::string> compile_keys;
    };

    void evaluateInto(
        GlobalMaterialId material,
        std::map<std::string, Entries, std::less<>> &passes,
        Additions &additions);
    PipelineHandle ensureOverride(
        GlobalMaterialId base, const MaterialDrawSubject &subject,
        const MaterialDrawStep &step, std::string_view node,
        Additions &additions);
    void release(const Additions &additions) noexcept;
    void retireAll() noexcept;

    std::uint64_t generation_ = 0;
    std::shared_ptr<const MaterialDrawLoopRules> rules_;
    std::map<std::string, Entries, std::less<>> passes_;
    // (base pipeline, override key) -> override pipeline.
    std::map<std::string, OverridePipeline> overrides_;
    // (base vertex, base fragment, sorted defines) -> compiled bundles.
    std::map<std::string, CompiledDefines> compiled_;
};

} // namespace Pelican
