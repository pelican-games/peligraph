#include "materialdrawloop.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace Pelican {

void throwMaterialDrawLoopRefusal(std::string_view code,
                                  const nlohmann::json &fields) {
    throw std::runtime_error(std::string{code} + ": " + fields.dump());
}

std::string materialDrawSubjectKey(
    std::string_view name, std::string_view source_document,
    std::string_view source_asset,
    std::optional<std::uint32_t> source_material_index) {
    if (!source_document.empty()) {
        return std::string{source_document} + "#" + std::string{name};
    }
    if (source_material_index) {
        return std::string{source_asset} + "#material[" +
               std::to_string(*source_material_index) + "]";
    }
    return std::string{name};
}

bool materialRouteIsTransparent(MaterialRouteClass route) noexcept {
    return route == MaterialRouteClass::forward_transparent;
}

void validateMaterialDrawStep(const MaterialDrawStep &step,
                              const MaterialDrawSubject &subject,
                              std::string_view node,
                              bool runtime_shader_compiler) {
    switch (step.kind) {
    case MaterialDrawStepKind::material:
        return;
    case MaterialDrawStepKind::variant:
        if (!std::binary_search(subject.variants.begin(),
                                subject.variants.end(), step.variant)) {
            throwMaterialDrawLoopRefusal(
                materialDrawVariantMissingCode,
                {{"node", node},
                 {"material", subject.key},
                 {"variant", step.variant}});
        }
        return;
    case MaterialDrawStepKind::override_pipeline:
        break;
    }
    if (step.render_state.blend) {
        const bool blends =
            *step.render_state.blend != SurfaceBlendMode::opaque;
        if (blends != materialRouteIsTransparent(subject.route)) {
            throwMaterialDrawLoopRefusal(
                materialDrawPhaseCrossingCode,
                {{"node", node}, {"material", subject.key}});
        }
    }
    if (!step.defines.empty() && !runtime_shader_compiler) {
        throwMaterialDrawLoopRefusal(
            materialDrawOverrideNeedsCompilerCode, {{"node", node}});
    }
}

SurfaceRenderState applyMaterialDrawRenderStateOverride(
    SurfaceRenderState base,
    const MaterialDrawRenderStateOverride &override_state) {
    if (override_state.blend) base.blend = *override_state.blend;
    if (override_state.cull) base.cull = *override_state.cull;
    if (override_state.depth_test) {
        base.depth_test = *override_state.depth_test;
    }
    if (override_state.depth_write) {
        base.depth_write = *override_state.depth_write;
    }
    if (override_state.depth_compare) {
        base.depth_compare = *override_state.depth_compare;
    }
    return base;
}

bool graphLoopTagFilterMatches(const GraphLoopTagFilter &filter,
                               const std::vector<std::string> &tags) {
    const auto has = [&](const std::string &tag) {
        return std::find(tags.begin(), tags.end(), tag) != tags.end();
    };
    if (!filter.include.empty() &&
        std::none_of(filter.include.begin(), filter.include.end(), has)) {
        return false;
    }
    if (std::any_of(filter.exclude.begin(), filter.exclude.end(), has)) {
        return false;
    }
    return std::all_of(filter.require_all.begin(), filter.require_all.end(),
                       has);
}

bool graphLoopDrawConditionMatches(const GraphLoopDrawCondition &condition,
                                   const MaterialDrawSubject &subject) {
    if (condition.material && *condition.material != subject.key) {
        return false;
    }
    if (condition.tags &&
        !graphLoopTagFilterMatches(*condition.tags, subject.tags)) {
        return false;
    }
    if (condition.contract &&
        *condition.contract != materialShaderContractName(subject.contract)) {
        return false;
    }
    if (condition.skinned && *condition.skinned != subject.skinned) {
        return false;
    }
    if (condition.has_variant &&
        *condition.has_variant == subject.variants.empty()) {
        return false;
    }
    if (condition.has_variant_named &&
        !std::binary_search(subject.variants.begin(), subject.variants.end(),
                            *condition.has_variant_named)) {
        return false;
    }
    return true;
}

namespace {

bool insideOver(const GraphDrawLoop &loop,
                const MaterialDrawSubject &subject) {
    return materialPassAcceptsMaterial(loop.contract, loop.pass, subject.route,
                                       subject.contract, subject.exact_pass);
}

} // namespace

std::optional<MaterialDrawLoopChoice> classifyGraphDrawLoopSubject(
    const GraphDrawLoop &loop, const MaterialDrawSubject &subject) {
    if (!insideOver(loop, subject)) return std::nullopt;
    if (loop.where && !graphLoopTagFilterMatches(*loop.where, subject.tags)) {
        return std::nullopt;
    }
    for (std::size_t branch = 0; branch < loop.branches.size(); ++branch) {
        const auto &entry = loop.branches[branch];
        if (!graphLoopDrawConditionMatches(entry.when, subject)) continue;
        return MaterialDrawLoopChoice{
            branch, !entry.steps || !entry.steps->empty()};
    }
    // The last branch is the else (graph.loop_branch_else_missing@1), so
    // this is reached only by a table built outside a graph.
    return std::nullopt;
}

MaterialDrawLoopPassRule makeGraphDrawLoopPassRule(GraphDrawLoop loop) {
    auto shared = std::make_shared<const GraphDrawLoop>(std::move(loop));
    MaterialDrawLoopPassRule rule;
    rule.node = shared->loop;
    rule.branch_count = shared->branches.size();
    rule.classify = [shared](const MaterialDrawSubject &subject) {
        return classifyGraphDrawLoopSubject(*shared, subject);
    };
    rule.rule = [shared](const MaterialDrawSubject &subject)
        -> std::optional<MaterialDrawStepList> {
        if (!insideOver(*shared, subject)) return std::nullopt;
        if (shared->where) {
            // What the queue filter (include / exclude) leaves out, the pass
            // never draws; require_all is the loop's own, so a material the
            // queue keeps is left undrawn here.
            auto queue = *shared->where;
            queue.require_all.clear();
            if (!graphLoopTagFilterMatches(queue, subject.tags)) {
                return std::nullopt;
            }
            if (!graphLoopTagFilterMatches(*shared->where, subject.tags)) {
                return MaterialDrawStepList{};
            }
        }
        const auto choice = classifyGraphDrawLoopSubject(*shared, subject);
        if (!choice) return std::nullopt;
        const auto &steps = shared->branches[choice->branch].steps;
        return steps ? *steps : MaterialDrawStepList{};
    };
    return rule;
}

void applyMaterialDrawLoopElementsToFramePlanJson(
    nlohmann::json &plan, const MaterialDrawLoopRules &rules,
    const std::vector<MaterialDrawSubject> &subjects) {
    const auto loops = plan.find("loops");
    if (loops == plan.end() || !loops->is_array()) return;
    for (auto &loop : *loops) {
        if (!loop.is_object() || loop.value("tier", std::string{}) != "draw_loop") {
            continue;
        }
        const auto pass = rules.passes.find(loop.value("pass", std::string{}));
        if (pass == rules.passes.end() || !pass->second.classify) continue;
        auto elements = nlohmann::json::array();
        std::vector<bool> taken(pass->second.branch_count, false);
        for (const auto &subject : subjects) {
            const auto choice = pass->second.classify(subject);
            if (!choice) continue;
            elements.push_back({{"element", subject.key},
                                {"index", elements.size()},
                                {"branch", choice->branch}});
            if (choice->branch < taken.size()) taken[choice->branch] = true;
        }
        auto unmatched = nlohmann::json::array();
        for (std::size_t branch = 0; branch < taken.size(); ++branch) {
            if (!taken[branch]) unmatched.push_back(branch);
        }
        loop["count"] = elements.size();
        loop["elements"] = std::move(elements);
        loop["unmatched_branches"] = std::move(unmatched);
    }
}

std::vector<std::size_t> materialDrawLoopUnservedSubjects(
    const MaterialDrawLoopRules &rules,
    const std::vector<MaterialDrawSubject> &subjects) {
    std::vector<std::size_t> unserved;
    for (std::size_t index = 0; index < subjects.size(); ++index) {
        for (const auto &[pass, rule] : rules.passes) {
            (void)pass;
            if (!rule.classify) continue;
            const auto choice = rule.classify(subjects[index]);
            if (choice && !choice->served) {
                unserved.push_back(index);
                break;
            }
        }
    }
    return unserved;
}

std::string materialDrawOverrideKey(const MaterialDrawStep &step) {
    std::ostringstream key;
    const auto optional_int = [&](const auto &value) {
        if (value) {
            key << static_cast<int>(*value);
        } else {
            key << '-';
        }
        key << ';';
    };
    optional_int(step.render_state.blend);
    optional_int(step.render_state.cull);
    optional_int(step.render_state.depth_test);
    optional_int(step.render_state.depth_write);
    optional_int(step.render_state.depth_compare);
    auto defines = step.defines;
    std::sort(defines.begin(), defines.end());
    defines.erase(std::unique(defines.begin(), defines.end()),
                  defines.end());
    for (const auto &define : defines) {
        key << define.size() << ':' << define;
    }
    return key.str();
}

} // namespace Pelican
