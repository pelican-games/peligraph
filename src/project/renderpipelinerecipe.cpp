#include "renderpipelinerecipe.hpp"
#include "canonicalanchorremoved.hpp"
#include "graphcomposition.hpp"
#include "renderconfigdocument.hpp"

#include <algorithm>
#include <set>

namespace Pelican {
namespace {
using Json = nlohmann::json;

[[noreturn]] void invalid(std::string_view code, std::string_view field,
                          std::string_view reason) {
    throw GraphRegistryError(std::string{code}, {{"field", field}, {"reason", reason}});
}

std::string stringField(const Json &object, std::string_view field, std::string_view code) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_string() || found->get_ref<const std::string &>().empty())
        invalid(code, field, "expected_nonempty_string");
    return found->get<std::string>();
}

struct Fragment {
    std::string family;
    RenderPipelineFragmentOrigin origin;
    Json document;
};

Fragment loadFragment(std::string family, const Json &reference,
                      const RenderPipelinePresetLoader &load_json) {
    constexpr auto code = "render_pipeline.fragment_invalid@1";
    if (!reference.is_string() || reference.get_ref<const std::string &>().empty())
        invalid(code, "reference", "expected_nonempty_string");
    const auto ref = reference.get<std::string>();
    auto document = parseRenderDocument(load_json(ref), ref);
    rejectCanonicalAnchorFields(document, {}, ref);
    if (!document.is_object()) invalid(code, "document", "expected_object");
    for (const auto &[key, value] : document.items()) {
        if (key != "schema" && key != "version" && key != "name" && key != "render_targets" &&
            key != "passes" && key != "required_targets")
            invalid(code, key, "unknown_key");
    }
    if (stringField(document, "schema", code) != "pelican.render_fragment")
        invalid(code, "schema", "unsupported_schema");
    if (!document.contains("version") || !document.at("version").is_number_integer() || document.at("version") != 1)
        invalid(code, "version", "expected_version_1");
    const auto name = stringField(document, "name", code);
    for (const auto *key : {"render_targets", "passes"}) {
        if (!document.contains(key) || !document.at(key).is_array()) invalid(code, key, "expected_array");
        for (const auto &declaration : document.at(key)) {
            if (!declaration.is_object()) invalid(code, key, "expected_object_declaration");
            (void)stringField(declaration, "name", code);
        }
    }
    if (document.contains("required_targets")) {
        if (!document.at("required_targets").is_array()) invalid(code, "required_targets", "expected_array");
        for (const auto &target : document.at("required_targets"))
            if (!target.is_string() || target.get_ref<const std::string &>().empty())
                invalid(code, "required_targets", "expected_nonempty_string");
    }
    return {std::move(family), {name, ref}, std::move(document)};
}
} // namespace

RenderPipelinePresetResolution expandRenderPipelineRecipe(
    const Json &config, const Json &overrides, const RenderPipelinePresetLoader &load_json) {
    constexpr auto code = "render_pipeline.recipe_invalid@1";
    for (const auto &[key, value] : config.items()) {
        if (key == "render_targets" || key == "rendering_passes" || key == "snapshots" ||
            key == "buffers" || key == "compute_tasks")
            throw GraphRegistryError("render_pipeline.recipe_has_structure@1", {{"key", key}});
        if (key != "base" && key != "features" && key != "graph_transforms" && key != "shader_defines" &&
            key != "draw_sort" && key != "material_routing" && key != "xr" && key != "render_strategy" &&
            key != "target_planning" && key != "vulkan_plan_pins" && key != "vulkan_physical_fragments")
            throw GraphRegistryError("render_pipeline.recipe_unknown_key@1", {{"key", key}});
    }
    if (!config.contains("base") || !config.at("base").is_array() || config.at("base").empty())
        invalid(code, "base", "expected_nonempty_array");
    if (!overrides.is_object()) invalid(code, "fragment_overrides", "expected_object");
    std::vector<Fragment> inherited;
    std::vector<std::string> names;
    std::set<std::string> families;
    RenderPipelinePresetResolution result;
    result.config = config;
    result.config.erase("base");
    result.config["render_targets"] = Json::array();
    result.config["rendering_passes"] = Json::array();
    for (const auto &base : config.at("base")) {
        if (!base.is_object()) invalid(code, "base", "expected_object");
        for (const auto &[key, value] : base.items())
            if (key != "family" && key != "fragments") invalid(code, key, "unknown_base_key");
        const auto family = stringField(base, "family", code);
        if (!families.insert(family).second) invalid(code, "family", "duplicate_family");
        if (!base.contains("fragments") || !base.at("fragments").is_array())
            invalid(code, "fragments", "expected_array");
        result.config["rendering_passes"].push_back({{"name", family}, {"passes", Json::array()}});
        for (const auto &ref : base.at("fragments")) {
            auto fragment = loadFragment(family, ref, load_json);
            if (std::find(names.begin(), names.end(), fragment.origin.name) != names.end())
                throw GraphRegistryError("render_pipeline.recipe_duplicate_fragment@1", {{"name", fragment.origin.name}});
            names.push_back(fragment.origin.name);
            inherited.push_back(std::move(fragment));
        }
    }
    for (const auto &[name, ref] : overrides.items())
        if (std::find(names.begin(), names.end(), name) == names.end())
            throw GraphRegistryError("render_pipeline.fragment_override_unknown@1", {{"name", name}, {"inherited", names}});
    std::vector<Fragment> effective;
    for (auto &fragment : inherited) {
        if (const auto replacement = overrides.find(fragment.origin.name); replacement != overrides.end()) {
            if (replacement->is_null()) continue;
            auto next = loadFragment(fragment.family, *replacement, load_json);
            if (next.origin.name != fragment.origin.name)
                throw GraphRegistryError("render_pipeline.fragment_override_name_mismatch@1",
                    {{"name", fragment.origin.name}, {"replacement_name", next.origin.name}});
            fragment = std::move(next);
        }
        effective.push_back(std::move(fragment));
    }
    std::set<std::string> targets;
    const auto record_origin = [](auto &origins, const std::string &name,
                                  const RenderPipelineFragmentOrigin &origin, std::string_view kind) {
        const auto [found, inserted] = origins.emplace(name, origin);
        if (!inserted)
            throw GraphRegistryError("render_pipeline.recipe_duplicate_name@1",
                {{"kind", kind}, {"name", name}, {"fragments", {found->second.name, origin.name}}});
    };
    for (const auto &fragment : effective) {
        for (const auto &target : fragment.document.at("render_targets")) {
            const auto name = target.at("name").get<std::string>();
            targets.insert(name);
            record_origin(result.fragment_targets, name, fragment.origin, "target");
            result.config["render_targets"].push_back(target);
        }
        auto &passes = *std::find_if(result.config["rendering_passes"].begin(), result.config["rendering_passes"].end(),
            [&](const auto &family) { return family.at("name") == fragment.family; });
        for (const auto &pass : fragment.document.at("passes")) {
            record_origin(result.fragment_passes, pass.at("name").get<std::string>(), fragment.origin, "pass");
            passes["passes"].push_back(pass);
        }
    }
    for (const auto &fragment : effective)
        if (fragment.document.contains("required_targets"))
            for (const auto &target : fragment.document.at("required_targets"))
                if (!targets.contains(target.get<std::string>()))
                    throw GraphRegistryError("render_pipeline.fragment_missing_target@1",
                        {{"fragment", fragment.origin.name}, {"target", target}});
    for (const auto &fragment : effective)
        rejectRemovedCanonicalAuthoring(fragment.document, fragment.origin.reference, &result.config);
    return result;
}
} // namespace Pelican
