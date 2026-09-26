#include "materialdrawlooptable.hpp"

#include "materialcontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shaderreflection.hpp"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace Pelican {

namespace {

// Never destroyed. A table owned by a renderer generation in the global
// module container is destroyed during static destruction; these statics are
// first used after that container is constructed, so they would be destroyed
// before it and the table's destructor would erase from a dead vector.
std::mutex &liveTablesMutex() {
    static auto *mutex = new std::mutex;
    return *mutex;
}

std::vector<MaterialDrawLoopTable *> &liveTables() {
    static auto *tables = new std::vector<MaterialDrawLoopTable *>;
    return *tables;
}

std::uint32_t maxReflectedSet(const ShaderReflection &reflection) {
    std::uint32_t result = 0;
    for (const auto &binding : reflection.bindings) {
        result = std::max(result, binding.set);
    }
    return result;
}

std::string describeBinding(const ShaderReflection &reflection,
                            std::uint32_t set, std::uint32_t binding) {
    std::string name;
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set == set && reflected.binding == binding) {
            name = reflected.name;
            break;
        }
    }
    std::ostringstream text;
    if (!name.empty()) text << name << ' ';
    text << "(set " << set << ", binding " << binding << ')';
    return text.str();
}

// The first place the override's pipeline layout differs from the base's:
// a set layout binding (type, count, stage flags) or the push constants.
// Equal set layouts are the same cached VkDescriptorSetLayout, so a
// descriptor set bound for the base stays valid under the override.
std::optional<std::string> firstLayoutDifference(
    const ShaderReflection &base, const ShaderReflection &candidate) {
    if (shaderReflectionUsesRayQueryFrameSet(base) !=
        shaderReflectionUsesRayQueryFrameSet(candidate)) {
        return describeBinding(base, PELICAN_SET_FRAME, 0);
    }
    const auto last_set =
        std::max(maxReflectedSet(base), maxReflectedSet(candidate));
    for (std::uint32_t set = 1; set <= last_set; ++set) {
        const auto left = makeDescriptorSetLayoutBindings(base, set);
        const auto right =
            makeDescriptorSetLayoutBindings(candidate, set);
        const auto size = std::max(left.size(), right.size());
        for (std::size_t index = 0; index < size; ++index) {
            if (index >= left.size()) {
                return describeBinding(candidate, set,
                                       right[index].binding);
            }
            if (index >= right.size()) {
                return describeBinding(base, set, left[index].binding);
            }
            if (left[index] == right[index]) continue;
            return left[index].binding <= right[index].binding
                       ? describeBinding(base, set, left[index].binding)
                       : describeBinding(candidate, set,
                                         right[index].binding);
        }
    }
    if (makePushConstantRanges(base) !=
        makePushConstantRanges(candidate)) {
        return std::string{"push_constant"};
    }
    return std::nullopt;
}

std::string joinedDefines(std::vector<std::string> defines) {
    std::sort(defines.begin(), defines.end());
    defines.erase(std::unique(defines.begin(), defines.end()),
                  defines.end());
    std::string result;
    for (const auto &define : defines) {
        result += std::to_string(define.size()) + ":" + define;
    }
    return result;
}

} // namespace

MaterialDrawLoopTable::MaterialDrawLoopTable(std::uint64_t generation)
    : generation_{generation} {
    const std::lock_guard lock{liveTablesMutex()};
    liveTables().push_back(this);
}

MaterialDrawLoopTable::~MaterialDrawLoopTable() {
    {
        const std::lock_guard lock{liveTablesMutex()};
        std::erase(liveTables(), this);
    }
    retireAll();
}

void MaterialDrawLoopTable::forEachLive(
    const std::function<void(MaterialDrawLoopTable &)> &visit) {
    std::vector<MaterialDrawLoopTable *> tables;
    {
        const std::lock_guard lock{liveTablesMutex()};
        tables = liveTables();
    }
    for (auto *table : tables) visit(*table);
}

void MaterialDrawLoopTable::materialRegisteredInLiveTables(
    GlobalMaterialId material) {
    forEachLive([&](MaterialDrawLoopTable &table) {
        table.evaluateMaterial(material);
    });
}

void MaterialDrawLoopTable::materialReleasedFromLiveTables(
    GlobalMaterialId material) noexcept {
    try {
        forEachLive([&](MaterialDrawLoopTable &table) {
            table.forgetMaterial(material);
        });
    } catch (...) {
    }
}

const std::vector<ResolvedMaterialDrawStep> *
MaterialDrawLoopTable::find(std::string_view pass,
                            GlobalMaterialId material) const {
    const auto entries = passes_.find(pass);
    if (entries == passes_.end()) return nullptr;
    const auto found = entries->second.find(material);
    return found == entries->second.end() ? nullptr : &found->second;
}

void MaterialDrawLoopTable::install(
    std::shared_ptr<const MaterialDrawLoopRules> rules) {
    auto previous_rules = std::move(rules_);
    auto previous_passes = std::move(passes_);
    auto previous_overrides = std::move(overrides_);
    auto previous_compiled = std::move(compiled_);
    rules_ = std::move(rules);
    passes_.clear();
    overrides_.clear();
    compiled_.clear();
    Additions additions;
    try {
        if (rules_ != nullptr && !rules_->empty()) {
            const auto &materials = GET_MODULE(MaterialContainer);
            for (const auto material : materials.drawLoopMaterials()) {
                evaluateInto(material, passes_, additions);
            }
        }
    } catch (...) {
        release(additions);
        rules_ = std::move(previous_rules);
        passes_ = std::move(previous_passes);
        overrides_ = std::move(previous_overrides);
        compiled_ = std::move(previous_compiled);
        throw;
    }
    // The previous overrides may still be recorded in a frame in flight;
    // the factory defers their destruction.
    std::swap(overrides_, previous_overrides);
    std::swap(compiled_, previous_compiled);
    retireAll();
    std::swap(overrides_, previous_overrides);
    std::swap(compiled_, previous_compiled);
}

void MaterialDrawLoopTable::evaluateMaterial(GlobalMaterialId material) {
    if (rules_ == nullptr || rules_->empty()) return;
    std::map<std::string, Entries, std::less<>> evaluated;
    Additions additions;
    try {
        evaluateInto(material, evaluated, additions);
    } catch (...) {
        release(additions);
        throw;
    }
    forgetMaterial(material);
    for (auto &[pass, entries] : evaluated) {
        for (auto &[id, steps] : entries) {
            passes_[pass][id] = std::move(steps);
        }
    }
}

void MaterialDrawLoopTable::forgetMaterial(
    GlobalMaterialId material) noexcept {
    for (auto entries = passes_.begin(); entries != passes_.end();) {
        entries->second.erase(material);
        if (entries->second.empty()) {
            entries = passes_.erase(entries);
        } else {
            ++entries;
        }
    }
}

void MaterialDrawLoopTable::evaluateInto(
    GlobalMaterialId material,
    std::map<std::string, Entries, std::less<>> &passes,
    Additions &additions) {
    const auto &materials = GET_MODULE(MaterialContainer);
    const auto subject = materials.drawLoopSubject(material);
    for (const auto &[pass, pass_rule] : rules_->passes) {
        if (!pass_rule.rule) continue;
        const auto steps = pass_rule.rule(subject);
        if (!steps) continue;
        std::vector<ResolvedMaterialDrawStep> resolved;
        resolved.reserve(steps->size());
        for (const auto &step : *steps) {
            validateMaterialDrawStep(
                step, subject, pass_rule.node,
                ShaderLibrary::runtimeCompilerAvailable());
            switch (step.kind) {
            case MaterialDrawStepKind::material:
                resolved.push_back({material, std::nullopt});
                break;
            case MaterialDrawStepKind::variant:
                resolved.push_back(
                    {materials.materialVariantResource(material,
                                                       step.variant),
                     std::nullopt});
                break;
            case MaterialDrawStepKind::override_pipeline:
                resolved.push_back(
                    {material, ensureOverride(material, subject, step,
                                              pass_rule.node,
                                              additions)});
                break;
            }
        }
        passes[pass][material] = std::move(resolved);
    }
}

PipelineHandle MaterialDrawLoopTable::ensureOverride(
    GlobalMaterialId base, const MaterialDrawSubject &subject,
    const MaterialDrawStep &step, std::string_view node,
    Additions &additions) {
    const auto &materials = GET_MODULE(MaterialContainer);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto base_pipeline = materials.materialPipeline(base);
    const auto key = std::to_string(base_pipeline.value) + "|" +
                     materialDrawOverrideKey(step);
    if (const auto found = overrides_.find(key);
        found != overrides_.end()) {
        return found->second.pipeline;
    }

    auto desc =
        materials.drawOverridePipelineDesc(base, step.render_state);
    std::optional<SurfaceShaderBundleIds> define_bundles;
    if (!step.defines.empty()) {
        if (!desc.frag) {
            throw std::logic_error(
                "material pipeline has no fragment shader");
        }
        const auto compile_key =
            std::to_string(desc.vert.value) + "|" +
            std::to_string(desc.frag->value) + "|" +
            joinedDefines(step.defines);
        auto compiled = compiled_.find(compile_key);
        if (compiled == compiled_.end()) {
            if (compiled_.size() >= materialDrawOverrideCompileLimit) {
                throwMaterialDrawLoopRefusal(
                    materialDrawOverrideCompileLimitCode,
                    {{"node", node},
                     {"count", compiled_.size() + 1},
                     {"limit", materialDrawOverrideCompileLimit}});
            }
            auto &shaders = GET_MODULE(ShaderLibrary);
            const auto bundles = shaders.compileWithAdditionalDefines(
                desc.vert, *desc.frag, step.defines);
            compiled = compiled_.emplace(compile_key,
                                         CompiledDefines{bundles})
                           .first;
            additions.compile_keys.push_back(compile_key);
        }
        desc.vert = compiled->second.bundles.vertex;
        desc.frag = compiled->second.bundles.fragment;
        define_bundles = compiled->second.bundles;

        // The layout is judged from the compiled stages before a pipeline
        // is built: the factory would refuse a resource interface the new
        // stages no longer reflect with a message of its own.
        auto &shaders = GET_MODULE(ShaderLibrary);
        const std::vector<ShaderReflection> stages{
            shaders.get(desc.vert).reflection,
            shaders.get(*desc.frag).reflection};
        if (const auto difference = firstLayoutDifference(
                pipeline_factory.reflection(base_pipeline),
                merge(stages))) {
            throwMaterialDrawLoopRefusal(
                materialDrawOverrideInterfaceCode,
                {{"node", node},
                 {"material", subject.key},
                 {"binding", *difference}});
        }
    }

    const auto pipeline = pipeline_factory.create(desc);
    overrides_.emplace(key, OverridePipeline{
                                .pipeline = pipeline,
                                .base_pipeline = base_pipeline,
                                .base_material = base,
                                .render_state = step.render_state,
                                .define_bundles = define_bundles,
                            });
    additions.override_keys.push_back(key);
    if (const auto difference = firstLayoutDifference(
            pipeline_factory.reflection(base_pipeline),
            pipeline_factory.reflection(pipeline))) {
        throwMaterialDrawLoopRefusal(
            materialDrawOverrideInterfaceCode,
            {{"node", node},
             {"material", subject.key},
             {"binding", *difference}});
    }
    return pipeline;
}

void MaterialDrawLoopTable::forEachOverride(
    const std::function<void(const OverridePipeline &)> &visit) const {
    for (const auto &[key, entry] : overrides_) {
        (void)key;
        visit(entry);
    }
}

void MaterialDrawLoopTable::release(const Additions &additions) noexcept {
    std::vector<PipelineHandle> pipelines;
    for (const auto &key : additions.override_keys) {
        const auto found = overrides_.find(key);
        if (found == overrides_.end()) continue;
        pipelines.push_back(found->second.pipeline);
        overrides_.erase(found);
    }
    std::vector<ShaderBundleId> bundles;
    for (const auto &key : additions.compile_keys) {
        const auto found = compiled_.find(key);
        if (found == compiled_.end()) continue;
        bundles.push_back(found->second.bundles.vertex);
        bundles.push_back(found->second.bundles.fragment);
        compiled_.erase(found);
    }
    if (auto *factory = FastModuleContainer::tryGet<PipelineFactory>()) {
        factory->retireRegistrations(pipelines);
    }
    if (auto *shaders = FastModuleContainer::tryGet<ShaderLibrary>()) {
        shaders->retireRegistrations(bundles);
    }
}

void MaterialDrawLoopTable::retireAll() noexcept {
    Additions everything;
    for (const auto &[key, pipeline] : overrides_) {
        (void)pipeline;
        everything.override_keys.push_back(key);
    }
    for (const auto &[key, compiled] : compiled_) {
        (void)compiled;
        everything.compile_keys.push_back(key);
    }
    release(everything);
}

} // namespace Pelican
