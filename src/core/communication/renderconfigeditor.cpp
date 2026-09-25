#include "../shader/raygrouplayout.hpp"
#include "renderconfigeditor.hpp"
#include "../../project/packagemanifest.hpp"
#include "../../project/rendercomponent.hpp"
#include "../../project/rasterrayquery.hpp"
#include "../../project/rasterpass.hpp"
#include "../../project/featurecompose.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/graphvariantpolicy.hpp"
#include "../../project/graphpartition.hpp"
#include "../../project/renderconfigsection.hpp"
#include "renderconfigtransaction.hpp"

#include "../fullscreenpass/fullscreeninputbindingmismatch.hpp"
#include "../loader/engineresources.hpp"
#include "../renderingpass/frameplanner.hpp"
#include "../renderingpass/passdefinitionjsonparser.hpp"
#include "../renderingpass/renderingpassjsonhelpers.hpp"
#include "../renderingpass/rendertargetjsonparser.hpp"
#include "../renderingpass/rendertargetmetadataresolver.hpp"
#include "../renderingpass/rendertargetnameresolver.hpp"
#include "../vkcore/renderer_config.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

#if PELICAN_RUNTIME_SHADER_COMPILER
constexpr bool runtime_shader_compiler_available = true;
#else
constexpr bool runtime_shader_compiler_available = false;
#endif

// G-4: the value a request asks material_route_default to take.
MaterialRouteDefault requestedMaterialRouteDefault(const Json &value,
                                                   std::string_view method) {
    if (value.is_string()) {
        const auto &text = value.get_ref<const std::string &>();
        if (text == "auto") return MaterialRouteDefault::automatic;
        if (text == "forward") return MaterialRouteDefault::forward;
    }
    throw std::invalid_argument(std::string{method} +
                                " material_route_default must be \"auto\" or \"forward\"");
}

// G-4: the codes whose "<code>: <JSON>" message the completion handler turns
// back into a named rejection with fields.
bool isCarriedRefusalCode(std::string_view code) {
    return code == materialRouteUnservedCode ||
           code == materialRouteDefaultUnavailableCode ||
           code == renderConfigLegacyTransactionDirCode;
}

// Q-14e: the fields a typed render_pipeline_preflight_failed refusal carries,
// added beside the ones a call site already has. Every refusal with that code
// that is built from an exception goes through here, whichever way the
// exception came (a catch in this file, or the renderer's apply result).
OrderedJson withPreflightRefusalFields(OrderedJson details,
                                       const std::exception &error) {
    if (const auto *mismatch =
            dynamic_cast<const FullscreenInputBindingMismatch *>(&error)) {
        const auto &info = mismatch->info();
        auto declared = OrderedJson::array();
        for (const auto &input : info.shader_pass_inputs) {
            declared.push_back(
                OrderedJson{{"binding", input.binding}, {"type", input.type}});
        }
        OrderedJson field{{"pass", info.pass},
                          {"fragment", info.fragment},
                          {"kind", info.kind},
                          {"binding", info.binding}};
        if (!info.resource.empty()) field["resource"] = info.resource;
        field["expected"] = info.expected;
        field["pass_texture_inputs"] = info.pass_texture_inputs;
        field["pass_buffer_inputs"] = info.pass_buffer_inputs;
        field["shader_pass_inputs"] = std::move(declared);
        if (details.is_null()) details = OrderedJson::object();
        details["shader_input_mismatch"] = std::move(field);
    }
    return details;
}

OrderedJson withPreflightRefusalFields(OrderedJson details,
                                       const std::exception_ptr &error) {
    if (!error) return details;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception &caught) {
        return withPreflightRefusalFields(std::move(details), caught);
    } catch (...) {
    }
    return details;
}

// A readable message for a carried refusal; the fields stay structured.
std::string carriedRefusalMessage(const CodedRefusal &refusal) {
    if (refusal.code == materialRouteUnservedCode) {
        return materialRouteUnservedText(refusal.fields);
    }
    return refusal.code + ": " +
           refusal.fields.value("message", refusal.fields.dump());
}

bool isEditableAuxiliaryPass(std::string_view type) {
    return type == "debug_draw" || type == "debug_text" || type == "gizmo" ||
           type == "shadow_depth" || type == "velocity" || type == "picking";
}

bool isEditablePassType(std::string_view type) {
    return type == "fullscreen" || type == "material" || type == "raster" ||
           isEditableAuxiliaryPass(type);
}

// One rule for "the editable declaration of this pass" in a
// `rendering_passes` list, whichever document holds the list: a project
// preset's `config` or, F-6, a direct root's own rendering section.
OrderedJson *findEditablePassIn(OrderedJson &rendering_passes, const std::string &graph_name,
                                const std::string &pass_name) {
    if (!rendering_passes.is_array()) return nullptr;
    for (auto &graph : rendering_passes) {
        if (!graph.is_object() || graph.value("name", std::string{}) != graph_name) continue;
        const auto passes = graph.find("passes");
        if (passes == graph.end() || !passes->is_array()) continue;
        for (auto &pass : *passes) {
            if (pass.is_object() && pass.value("name", std::string{}) == pass_name &&
                isEditablePassType(pass.value("type", std::string{}))) return &pass;
        }
    }
    return nullptr;
}

OrderedJson *findPresetEditablePass(OrderedJson &preset, const std::string &graph_name,
                                      const std::string &pass_name) {
    return findEditablePassIn(preset.at("config").at("rendering_passes"), graph_name, pass_name);
}

// F-6: the pass a direct root declares itself, found in the root's rendering
// section. Null for a preset root's passes, and for anything the root does not
// declare (a feature's pass, or what the engine adds such as output_transform).
// The root is a pelican.graph root (MG-2b: anything else is refused as
// render_config.legacy_root@1 before it becomes the editor's document), so its
// passes are only ever in its `rendering` section (MG-3b).
OrderedJson *findRootEditablePass(OrderedJson &root, const std::string &graph_name,
                                  const std::string &pass_name) {
    if (!root.is_object()) return nullptr;
    const auto rendering = root.find("rendering");
    if (rendering == root.end() || !rendering->is_object()) return nullptr;
    const auto passes = rendering->find("rendering_passes");
    return passes == rendering->end() ? nullptr : findEditablePassIn(*passes, graph_name, pass_name);
}

OrderedJson &featureSourceFragment(OrderedJson &feature, std::string_view provider) {
    const auto name = feature.at("name").get<std::string>();
    if (provider == name) return feature;
    OrderedJson *found = nullptr;
    if (auto integrations = feature.find("integrations"); integrations != feature.end()) {
        for (auto &integration : *integrations) {
            if (name + "/" + integration.at("name").get<std::string>() != provider) continue;
            if (found) throw std::runtime_error("The feature has ambiguous integration source declarations");
            found = &integration.at("fragment");
        }
    }
    if (!found) throw std::runtime_error("The node's feature source fragment is no longer present");
    return *found;
}

OrderedJson *findFeatureEditablePass(OrderedJson &feature, std::string_view name, std::string_view provider) {
    if (feature.value("schema", std::string{}) != "pelican.render_feature") return nullptr;
    // A materialized feature keeps its scalar declarations (P-2a-1c) so its
    // parameters stay editable, but its passes are already bound and are
    // edited here.  Only an unbound declaration still has to be edited where
    // it was authored.
    if (feature.contains("parameters") && !feature.contains("bound_parameters"))
        throw std::runtime_error("Parameterized feature passes must be edited in their source document.");
    auto &fragment = featureSourceFragment(feature, provider);
    const auto entries = fragment.find("passes");
    if (entries == fragment.end() || !entries->is_array()) return nullptr;
    OrderedJson *found = nullptr;
    for (auto &entry : *entries) {
        if (!entry.is_object() || !entry.contains("pass") || !entry.at("pass").is_object()) continue;
        auto &pass = entry.at("pass");
        const auto type = pass.value("type", std::string{});
        if (pass.value("name", std::string{}) != name ||
            !isEditablePassType(type)) continue;
        if (found) throw std::runtime_error("The feature has ambiguous source pass declarations");
        found = &pass;
    }
    return found;
}

void requireObject(const Json &params, std::string_view method) {
    if (!params.is_object()) {
        throw std::invalid_argument(std::string{method} +
                                    " params must be an object");
    }
}

void requireOnly(const Json &params,
                 std::initializer_list<std::string_view> allowed,
                 std::string_view method) {
    requireObject(params, method);
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) ==
            allowed.end()) {
            throw std::invalid_argument(std::string{method} +
                                        " contains unsupported field '" +
                                        key + "'");
        }
    }
}

std::string requireString(const Json &object, std::string_view field,
                          std::string_view method) {
    const auto found = object.find(std::string{field});
    if (found == object.end() || !found->is_string()) {
        throw std::invalid_argument(std::string{method} +
                                    " requires string field '" +
                                    std::string{field} + "'");
    }
    return found->get<std::string>();
}

bool isLowerHexDigest(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') ||
                      (ch >= 'a' && ch <= 'f');
           });
}

std::string readFileBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error(
            "could not open render config source: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

OrderedJson digestJson(std::string_view digest) {
    return {{"algorithm", "sha256"}, {"hex", digest}};
}

struct GeometrySpaceDocument {
    OrderedJson info{{"space", "geometry"}, {"editable", false},
                     {"source_reference", ""}, {"source_digest", digestJson("")}};
    std::filesystem::path path;
    std::string bytes;
};

GeometrySpaceDocument geometrySpaceDocument(
    const Json &config, const RenderConfigEditorDependencies &dependencies, bool procedural = false) {
    GeometrySpaceDocument result;
    const auto reference = config.find("ray_group_layout");
    if (reference == config.end()) {
        result.info["reason"] = "Coordinate space editing requires a project ray group layout.";
        return result;
    }
    try {
        const auto source = reference->get<std::string>();
        result.info["source_reference"] = source;
        const bool engine = source.starts_with("engine://");
        if (engine) {
            result.bytes = engineResourceOrThrow(source.substr(9));
        } else {
            if (!dependencies.resolve_document_path || !dependencies.normalize_reference)
                throw std::runtime_error("Project document access is unavailable.");
            result.path = dependencies.resolve_document_path(source);
            result.bytes = readFileBytes(result.path);
        }
        result.info["source_digest"] = digestJson(renderConfigSourceDigest(result.bytes));
        const auto layout = parseRayGroupLayout(result.bytes, source);
        result.info["space"] = layout.acceleration_structure && (procedural
                                   ? layout.acceleration_structure->procedural_world_space
                                   : layout.acceleration_structure->triangle_world_space)
                                   ? "world" : "geometry";
        if (engine) result.info["reason"] = "Engine ray group layouts cannot be edited here.";
        else if (procedural &&
                 !layout.engineRecordOffset(RayEngineRecordField::procedural_transform))
            result.info["reason"] = "AABB space requires engine_record procedural_transform and shaders using the procedural transform helpers.";
        else result.info["editable"] = true;
    } catch (const std::exception &error) {
        result.info["reason"] = error.what();
    }
    return result;
}

OrderedJson runtimeJson(const RenderConfigRuntimeSnapshot &runtime) {
    return {{"published_generation", runtime.published_generation},
            {"enabled_feature_names", runtime.enabled_feature_names}};
}

std::string primaryReason(const std::vector<std::string> &reasons) {
    return reasons.empty() ? std::string{"editor_gate_closed"}
                           : reasons.front();
}

// P-2a-4a decided where an authored key lives; P-2a-4b moved the rule out to
// `project/renderconfigsection.hpp` (same namespace) so the engine-side
// parsers, the overlay, `devcli project init` and the tests all read the one
// locator instead of each keeping a copy. The unqualified calls below resolve
// to it.

constexpr std::string_view managedFragmentDirectory =
    "project://passes/authoring/";
constexpr std::string_view managedMarkerField =
    "pelican_editor_managed";

std::vector<std::string> allFeatureReferences(std::string_view bytes) {
    const auto document = Json::parse(bytes);
    const auto &root = renderingSectionOrEmpty(document);
    std::vector<std::string> result;
    const auto features = root.find("features");
    if (features == root.end()) return result;
    if (!features->is_array()) {
        throw std::invalid_argument(
            "render config requires a top-level features array");
    }
    for (const auto &entry : *features) {
        if (entry.is_string()) {
            result.push_back(entry.get<std::string>());
        } else if (entry.is_object()) {
            const auto ref = entry.find("ref");
            if (ref != entry.end() && ref->is_string()) {
                result.push_back(ref->get<std::string>());
            }
        }
    }
    return result;
}

bool hasManagedFragmentMarker(std::string_view bytes) {
    try {
        const auto feature = Json::parse(bytes);
        if (!feature.is_object() ||
            feature.value("schema", std::string{}) !=
                "pelican.render_feature" ||
            feature.value("version", 0) != 1) {
            return false;
        }
        const auto marker = feature.find(std::string{managedMarkerField});
        return marker != feature.end() && marker->is_object() &&
               marker->value("schema", std::string{}) ==
                   "pelican.authored_render_fragment" &&
               marker->value("version", 0) == 1 &&
               marker->value("owner", std::string{}) ==
                   "pelican.devstudio" &&
               marker->contains("id") && marker->at("id").is_string();
    } catch (const Json::exception &) {
        return false;
    }
}

std::string authoredPassId(std::string_view graph,
                           std::string_view pass_name) {
    std::string identity{graph};
    identity.push_back('\0');
    identity.append(pass_name);
    return renderConfigSourceDigest(identity).substr(0, 20);
}

std::string managedFragmentReference(std::string_view id) {
    return std::string{managedFragmentDirectory} + "pass-" +
           std::string{id} + ".json";
}

std::string managedFragmentBytes(std::string_view id,
                                 std::string_view insert,
                                 const Json &pass) {
    OrderedJson fragment{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "authored_pass_" + std::string{id}},
        {"runtime_shader_compiler", "optional"},
        {std::string{managedMarkerField},
         {{"schema", "pelican.authored_render_fragment"},
          {"version", 1},
          {"owner", "pelican.devstudio"},
          {"id", id}}},
        {"passes",
         OrderedJson::array(
             {{{"insert", insert}, {"pass", pass}}})},
    };
    return fragment.dump(2) + "\n";
}

// G-1. A material pass is judged by the pass parser the pipeline load runs --
// contract, color count, depth, formats, no swapchain output -- against the
// targets of the composed candidate, so a shape the load would refuse is
// refused before anything is written, in the parser's own words. Composition
// alone does not look at attachment formats. The screen extent is nominal:
// only the ratios between one pass's outputs matter to the parser.
void validateAuthoredMaterialPass(const Json &config, const std::string &graph,
                                  const std::string &pass_name, bool hdr) {
    std::optional<Json> pass;
    for (const auto &candidate : config.value("rendering_passes", Json::array())) {
        if (!candidate.is_object() || candidate.value("name", std::string{}) != graph)
            continue;
        for (const auto &entry : candidate.value("passes", Json::array()))
            if (entry.is_object() && entry.value("name", std::string{}) == pass_name)
                pass = entry;
    }
    if (!pass)
        throw std::runtime_error("the composed candidate has no pass " + pass_name +
                                 " in " + graph);
    constexpr vk::Extent2D screen{1920, 1080};
    const auto definitions = parseRenderTargetDefinitionsFromJson(
        resolveRenderTargetFormatClassesV2(
            Json{{"resolver_version", 2},
                 {"render_targets", config.value("render_targets", Json::array())}},
            vk::Format::eB8G8R8A8Srgb, screen, hdr));
    std::unordered_set<std::string> buffers;
    for (const auto &buffer : config.value("buffers", Json::array()))
        if (buffer.is_object() && buffer.contains("name") && buffer.at("name").is_string())
            buffers.insert(buffer.at("name").get<std::string>());
    const RenderTargetNameResolver names{[&](const std::string &name) {
        for (std::size_t index = 0; index < definitions.size(); ++index)
            if (definitions[index].name == name)
                return GlobalRenderTargetId{static_cast<int>(index)};
        return noRenderTargetId();
    }};
    const RenderTargetMetadataResolver metadata{[&](GlobalRenderTargetId id) {
        if (id.value < 0 || static_cast<std::size_t>(id.value) >= definitions.size())
            throw std::runtime_error("render target metadata requested for an unknown target");
        const auto &definition = definitions[static_cast<std::size_t>(id.value)];
        const auto scaled = [&](std::uint32_t size) {
            return std::max<std::uint32_t>(
                1, static_cast<std::uint32_t>(size * definition.extent_scale + 0.5f));
        };
        const auto extent = definition.fixed_extent.value_or(
            vk::Extent2D{scaled(screen.width), scaled(screen.height)});
        return RenderTargetMetadata{
            .name = definition.name,
            .usage = definition.usage,
            .format = definition.format,
            .extent = extent,
            .history = definition.history,
            .samples = definition.samples,
            .mip_levels = definition.mip_levels.mode == ImageMipLevelMode::full_chain
                              ? maximumImageMipLevels(extent.width, extent.height)
                              : definition.mip_levels.count,
            .array_layers = definition.array_layers,
            .dimension = definition.dimension,
            .storage_mode = definition.storage_mode,
        };
    }};
    (void)parsePassDefinitionFromJson(*pass, names, metadata, buffers);
}

std::string provenanceSourceName(RenderPipelineProvenanceSource source) {
    switch (source) {
    case RenderPipelineProvenanceSource::project: return "project";
    case RenderPipelineProvenanceSource::feature: return "feature";
    case RenderPipelineProvenanceSource::engine: return "engine";
    }
    return "engine";
}

// P-2a-3: an imported component is read from outside the project.  A path
// inside it is refused by name so the one way to reference a document in the
// project stays `project://`.
bool isWithinDirectory(const std::filesystem::path &root,
                       const std::filesystem::path &candidate) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    const auto canonical_candidate =
        std::filesystem::weakly_canonical(candidate, error);
    if (error) return false;
    auto root_it = canonical_root.begin();
    auto candidate_it = canonical_candidate.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == canonical_candidate.end() ||
            *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

bool isManagedFragmentPath(const std::filesystem::path &project_root,
                           const std::filesystem::path &path) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        project_root / "passes" / "authoring", error);
    if (error) return false;
    const auto candidate = std::filesystem::weakly_canonical(path, error);
    if (error) return false;
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() ||
            *root_it != *candidate_it) {
            return false;
        }
    }
    return candidate_it != candidate.end();
}

// --- F-1: render targets authored from the editor.

// The one managed fragment every editor-authored render target lives in. It is
// a feature because a feature is the one place a target can be declared under
// both a direct root and a preset root (a preset root takes no render_targets).
constexpr std::string_view renderTargetFragmentId = "render-targets";

std::string renderTargetFragmentReference() {
    return std::string{managedFragmentDirectory} +
           std::string{renderTargetFragmentId} + ".json";
}

bool isReservedRenderTargetName(std::string_view name) {
    return name == "display" || name == "swapchain";
}

std::vector<std::string> renderTargetNames(const Json &section) {
    std::vector<std::string> names;
    if (!section.is_object()) return names;
    const auto targets = section.find("render_targets");
    if (targets == section.end() || !targets->is_array()) return names;
    for (const auto &target : *targets) {
        if (target.is_object() && target.contains("name") &&
            target.at("name").is_string())
            names.push_back(target.at("name").get<std::string>());
    }
    return names;
}

OrderedJson fragmentRenderTargets(std::string_view bytes) {
    const auto fragment = OrderedJson::parse(bytes);
    const auto targets = fragment.find("render_targets");
    if (targets == fragment.end()) return OrderedJson::array();
    if (!targets->is_array())
        throw std::runtime_error(
            "the managed render target fragment's render_targets is not an array");
    return *targets;
}

std::string renderTargetFragmentBytes(std::optional<std::string_view> existing,
                                      OrderedJson targets) {
    OrderedJson fragment =
        existing ? OrderedJson::parse(*existing)
                 : OrderedJson{
                       {"schema", "pelican.render_feature"},
                       {"version", 1},
                       {"name", "authored_render_targets"},
                       {"runtime_shader_compiler", "optional"},
                       {std::string{managedMarkerField},
                        {{"schema", "pelican.authored_render_fragment"},
                         {"version", 1},
                         {"owner", "pelican.devstudio"},
                         {"id", renderTargetFragmentId}}},
                   };
    fragment["render_targets"] = std::move(targets);
    return fragment.dump(2) + "\n";
}

// Where one composed render target was declared. The document reads come
// first (the fragment, the root section, the preset's config) because they are
// exact; the composer's provenance answers for features and for what the
// engine added. A name nothing answers for is "composed" -- said, not guessed.
struct RenderTargetOrigins {
    std::set<std::string, std::less<>> authoring;
    std::set<std::string, std::less<>> root;
    std::set<std::string, std::less<>> preset;
    std::map<std::string, RenderResourceProvenance, std::less<>> provenance;

    [[nodiscard]] std::string source(const std::string &name) const {
        if (name == "display") return "engine";
        if (authoring.contains(name)) return "authoring";
        if (const auto found = provenance.find(name);
            found != provenance.end() &&
            found->second.source == RenderPipelineProvenanceSource::feature) {
            return "feature:" + (found->second.provider_reference.empty()
                                     ? found->second.provider_feature
                                     : found->second.provider_reference);
        }
        if (root.contains(name)) return "root";
        if (preset.contains(name)) return "preset";
        if (const auto found = provenance.find(name);
            found != provenance.end() &&
            found->second.source == RenderPipelineProvenanceSource::engine)
            return "engine";
        return "composed";
    }
};

RenderTargetOrigins renderTargetOrigins(
    const ResolvedRenderConfigAuthoringContext &resolved,
    std::string_view root_bytes, const OrderedJson &fragment_targets) {
    RenderTargetOrigins origins;
    for (const auto &target : fragment_targets) {
        if (target.is_object() && target.contains("name") &&
            target.at("name").is_string())
            origins.authoring.insert(target.at("name").get<std::string>());
    }
    try {
        for (auto &name :
             renderTargetNames(renderingSectionOrEmpty(Json::parse(root_bytes))))
            origins.root.insert(std::move(name));
    } catch (const std::exception &) {
        // An unreadable root has no targets to claim.
    }
    if (resolved.pipeline_preset_bytes) {
        try {
            const auto preset = Json::parse(*resolved.pipeline_preset_bytes);
            for (auto &name :
                 renderTargetNames(preset.value("config", Json::object())))
                origins.preset.insert(std::move(name));
        } catch (const std::exception &) {
        }
    }
    for (const auto &entry : resolved.resource_provenance) {
        if (entry.kind == "render_target")
            origins.provenance.insert_or_assign(entry.name, entry);
    }
    return origins;
}

// Who still reads or writes each render target: the planner's own reading of
// the composed passes and tasks, not a second scanner of pass fields.
std::map<std::string, std::vector<std::string>, std::less<>>
renderTargetUsers(const Json &composed) {
    std::map<std::string, std::vector<std::string>, std::less<>> users;
    const auto add = [&users](std::string resource, const std::string &node) {
        if (const auto at = resource.find('@'); at != std::string::npos)
            resource.erase(at);
        auto &names = users[resource];
        if (std::find(names.begin(), names.end(), node) == names.end())
            names.push_back(node);
    };
    for (const auto &graph : parseFrameGraphDefinitionsFromConfigJson(composed)) {
        for (const auto &node : graph.nodes) {
            for (const auto &resource : node.reads) add(resource, node.name);
            for (const auto &resource : node.reads_history) add(resource, node.name);
            for (const auto &resource : node.writes) add(resource, node.name);
            for (const auto &access : node.resource_accesses)
                add(access.resource, node.name);
        }
    }
    return users;
}

} // namespace

std::vector<RenderFeatureCatalogEntry>
enumerateEngineRenderFeatureDocuments(
    const std::function<RenderFeatureRuntimeAvailability(
        std::string_view, const Json &)> &availability) {
    if (!availability) {
        throw std::invalid_argument(
            "render feature enumeration requires an engine availability evaluator");
    }
    std::vector<RenderFeatureCatalogEntry> result;
    std::set<std::string, std::less<>> names;
    for (const auto id : registeredEngineResourceIds()) {
        if (!id.starts_with("features/") || !id.ends_with(".json")) {
            continue;
        }
        const auto resource = engineResource(id);
        if (!resource) continue;
        const auto document = Json::parse(*resource);
        if (!document.is_object() ||
            document.value("schema", std::string{}) !=
                "pelican.render_feature") {
            continue;
        }
        if (!document.contains("name") ||
            !document.at("name").is_string()) {
            throw std::runtime_error(
                "registered render feature has no string name: " +
                std::string{id});
        }
        const auto name = document.at("name").get<std::string>();
        if (!names.insert(name).second) {
            throw std::runtime_error(
                "registered render feature name is duplicated: " + name);
        }
        auto evaluated = availability(name, document);
        if (!evaluated.available &&
            evaluated.unavailable_reason.empty()) {
            throw std::runtime_error(
                "engine marked render feature unavailable without a reason: " +
                name);
        }
        if (evaluated.available) {
            evaluated.unavailable_reason.clear();
        }
        result.push_back(RenderFeatureCatalogEntry{
            .name = name,
            .reference = "engine://" + std::string{id},
            .requires_runtime_module =
                renderFeatureRequiresRuntimeModule(name),
            .available = evaluated.available,
            .unavailable_reason =
                std::move(evaluated.unavailable_reason),
        });
    }
    std::ranges::sort(result, {}, &RenderFeatureCatalogEntry::name);
    return result;
}

std::vector<RenderFeatureCatalogEntry>
enumeratePackageRenderFeatureDocuments(
    const std::filesystem::path &project_root,
    const std::function<RenderFeatureRuntimeAvailability(
        std::string_view, const Json &)> &availability) noexcept {
    std::vector<RenderFeatureCatalogEntry> result;
    if (project_root.empty() || !availability) return result;
    std::vector<InstalledPackage> installed;
    try {
        installed = readInstalledPackages(project_root);
    } catch (...) {
        // A project with no project.json, or one the editor cannot read, has
        // no packages.  The catalog is still built.
        return result;
    }
    for (const auto &package : installed) {
        std::vector<PackageFeatureDocument> documents;
        try {
            documents = readPackageFeatureDocuments(project_root,
                                                    package.name_space);
        } catch (const std::exception &error) {
            result.push_back(RenderFeatureCatalogEntry{
                .name = package.name_space,
                .reference = packageReferencePrefix(package.name_space),
                .requires_runtime_module = false,
                .available = false,
                .unavailable_reason = error.what(),
                .source = "package",
            });
            continue;
        } catch (...) {
            continue;
        }
        for (auto &document : documents) {
            RenderFeatureCatalogEntry entry{
                .name = document.name,
                .reference = document.reference,
                .requires_runtime_module = false,
                .available = false,
                .unavailable_reason = document.error,
                .source = "package",
            };
            if (document.error.empty()) {
                try {
                    const auto parsed = Json::parse(document.bytes);
                    entry.requires_runtime_module =
                        renderFeatureRequiresRuntimeModule(entry.name);
                    auto evaluated = availability(entry.name, parsed);
                    entry.available = evaluated.available;
                    entry.unavailable_reason =
                        evaluated.available
                            ? std::string{}
                            : (evaluated.unavailable_reason.empty()
                                   ? std::string{
                                         "the engine reported no reason"}
                                   : evaluated.unavailable_reason);
                } catch (const std::exception &error) {
                    entry.available = false;
                    entry.unavailable_reason = error.what();
                } catch (...) {
                    entry.available = false;
                    entry.unavailable_reason =
                        "unknown package feature failure";
                }
            }
            result.push_back(std::move(entry));
        }
    }
    std::ranges::sort(result, {}, &RenderFeatureCatalogEntry::name);
    return result;
}

std::vector<RenderFeatureCatalogEntry>
composeRenderFeatureCatalog(
    std::vector<RenderFeatureCatalogEntry> engine_entries,
    std::vector<RenderFeatureCatalogEntry> package_entries) {
    std::set<std::string, std::less<>> names;
    for (const auto &entry : engine_entries) names.insert(entry.name);
    for (auto &entry : package_entries) {
        if (!names.insert(entry.name).second) {
            // The engine enumerator throws on a duplicate.  A package cannot
            // be allowed to do that, so the later entry is reported instead.
            entry.available = false;
            entry.unavailable_reason =
                "another render feature already uses this name: " + entry.name;
        }
        engine_entries.push_back(std::move(entry));
    }
    return engine_entries;
}

RenderConfigEditorService::RenderConfigEditorService(
    RenderConfigEditorDependencies dependencies)
    : dependencies_{std::move(dependencies)},
      document_{AuthoredRenderConfigDocument::inspect(
          dependencies_.source_bytes)} {
    if (dependencies_.source_reference.empty() ||
        dependencies_.source_path.empty() || !dependencies_.gate ||
        !dependencies_.runtime_snapshot || !dependencies_.apply_candidate) {
        throw std::invalid_argument(
            "RenderConfigEditorService requires source, gate, runtime and apply dependencies");
    }
    if (!dependencies_.feature_catalog) {
        throw std::invalid_argument(
            "RenderConfigEditorService requires an engine-evaluated feature catalog");
    }
    if (dependencies_.project_root.empty()) {
        dependencies_.project_root =
            dependencies_.source_path.parent_path();
    }
    if (dependencies_.normalize_reference) {
        source_normalized_reference_ =
            dependencies_.normalize_reference(
                dependencies_.source_reference);
    } else {
        std::error_code error;
        source_normalized_reference_ =
            "file:" +
            std::filesystem::weakly_canonical(
                dependencies_.source_path, error)
                .generic_string();
        if (error) {
            source_normalized_reference_ =
                "file:" + dependencies_.source_path.generic_string();
        }
    }
    feature_catalog_ = dependencies_.feature_catalog();
    loadManagedDocumentBaselines();
}

RenderConfigEditorService::~RenderConfigEditorService() {
    lifetime_.reset();
}

void RenderConfigEditorService::loadManagedDocumentBaselines() {
    managed_documents_.clear();
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path) {
        return;
    }
    for (const auto &reference : allFeatureReferences(document_.bytes())) {
        try {
            const auto path =
                dependencies_.resolve_document_path(reference);
            if (!isManagedFragmentPath(dependencies_.project_root, path)) {
                continue;
            }
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error) {
                continue;
            }
            auto bytes = readFileBytes(path);
            if (!hasManagedFragmentMarker(bytes)) continue;
            auto normalized =
                dependencies_.normalize_reference(reference);
            const auto digest = renderConfigSourceDigest(bytes);
            managed_documents_.insert_or_assign(
                normalized,
                ManagedDocumentBaseline{
                    .reference = reference,
                    .normalized_reference = normalized,
                    .path = path,
                    .bytes = std::move(bytes),
                    .digest = digest,
                });
        } catch (const std::exception &) {
            // Engine/fragment-address/manual references are not owned by this
            // transaction service and remain PathResolver fallbacks.
        }
    }
}

RenderConfigCandidateDocumentSet
RenderConfigEditorService::currentDocumentSet() const {
    std::vector<RenderConfigCandidateDocument> documents;
    documents.reserve(managed_documents_.size() + 1);
    documents.push_back(RenderConfigCandidateDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .operation = RenderConfigDocumentOperation::replace,
        .expected =
            {RenderConfigDocumentExistence::present,
             document_.sourceDigest()},
        .bytes = document_.bytes(),
    });
    for (const auto &[key, managed] : managed_documents_) {
        (void)key;
        documents.push_back(RenderConfigCandidateDocument{
            .reference = managed.reference,
            .normalized_reference = managed.normalized_reference,
            .path = managed.path,
            .operation = RenderConfigDocumentOperation::replace,
            .expected =
                {RenderConfigDocumentExistence::present,
                 managed.digest},
            .bytes = managed.bytes,
        });
    }
    return RenderConfigCandidateDocumentSet{
        source_normalized_reference_, std::move(documents)};
}

RenderConfigEditorService::GateSnapshot
RenderConfigEditorService::gateSnapshot() {
    const auto observation = dependencies_.gate();
    const auto epoch = gate_tracker_.observe(observation);
    return {.can_edit = observation.can_edit,
            .epoch = epoch,
            .reasons = observation.reasons};
}

OrderedJson RenderConfigEditorService::rejection(
    std::string code, std::string message, OrderedJson details) const {
    details["code"] = std::move(code);
    details["message"] = std::move(message);
    return {{"status", "rejected"},
            {"committed", false},
            {"error", std::move(details)}};
}

OrderedJson RenderConfigEditorService::preflightRejection(
    const std::exception &error, OrderedJson details) const {
    return rejection("render_pipeline_preflight_failed", error.what(),
                     withPreflightRefusalFields(std::move(details), error));
}

std::optional<OrderedJson> RenderConfigEditorService::preflightGraphVariants(
    const RenderConfigCandidateDocumentSet &candidate,
    OrderedJson details) const {
    if (!dependencies_.preflight_graph_variants) return std::nullopt;
    try {
        dependencies_.preflight_graph_variants(candidate);
    } catch (const GraphVariantCompositionError &error) {
        details["variant"] =
            std::string{renderPipelineGraphVariantName(error.variant())};
        return preflightRejection(error, std::move(details));
    }
    return std::nullopt;
}

const RenderFeatureCatalogEntry *
RenderConfigEditorService::findCatalogReference(
    std::string_view reference) const noexcept {
    const auto found = std::find_if(
        feature_catalog_.begin(), feature_catalog_.end(),
        [&](const auto &entry) { return entry.reference == reference; });
    return found == feature_catalog_.end() ? nullptr : &*found;
}

OrderedJson RenderConfigEditorService::getRenderFeatures(
    const Json &params) const {
    requireOnly(params, {}, "get_render_features");
    const auto uneditable_entries =
        document_.uneditableFeatureEntryCount();
    auto instances = OrderedJson::array();
    const auto root = Json::parse(document_.bytes());
    const auto append_instances = [&](const Json &features, std::string_view scope,
                                      const std::string &source_reference, const std::string &source_digest,
                                      bool source_editable) {
    for (std::size_t index = 0; index < features.size(); ++index) {
        const auto &entry = features.at(index);
        OrderedJson instance{{"index", index}, {"reference", ""}, {"parameters", Json::object()},
                             {"parameter_schema", nullptr}, {"editable", false}, {"removable", scope == "root" && entry.is_string()},
                             {"scope", scope}, {"source_reference", source_reference}, {"source_digest", digestJson(source_digest)}};
        try {
            const auto reference = entry.is_string() ? entry.get<std::string>() : entry.at("ref").get<std::string>();
            instance["reference"] = reference;
            if (entry.is_object()) instance["parameters"] = entry.value("parameters", Json::object());
            std::string bytes;
            if (reference.starts_with("engine://")) bytes = engineResourceOrThrow(reference.substr(9));
            else if (dependencies_.resolve_document_path) bytes = readFileBytes(dependencies_.resolve_document_path(reference));
            else throw std::runtime_error("Project feature document access is unavailable.");
            const auto feature = Json::parse(bytes);
            instance["feature_document_digest"] = digestJson(renderConfigSourceDigest(bytes));
            instance["materializable"] = source_editable && !feature.contains("bound_parameters");
            if (!source_editable) instance["materialize_reason"] = "Fork the engine preset into the project first.";
            else if (feature.contains("bound_parameters")) instance["materialize_reason"] = "This feature is already materialized in the project.";
            instance["parameter_schema"] = feature.value("parameters", Json(nullptr));
            // A materialized feature keeps its declarations and carries the
            // values it was cut with.  Those values are this derivative's
            // defaults, so the schema reports them as such and readers that
            // only know the schema (the Studio dialog) show the current value
            // instead of the engine default.
            if (const auto bound = feature.find("bound_parameters");
                bound != feature.end() && bound->is_object()) {
                instance["bound_parameters"] = *bound;
                if (instance.at("parameter_schema").is_object()) {
                    auto &schema = instance.at("parameter_schema");
                    for (const auto *category : {"scalars", "render_targets", "shader_assets"}) {
                        const auto declarations = schema.find(category);
                        if (declarations == schema.end() || !declarations->is_array()) continue;
                        for (auto &declaration : *declarations) {
                            if (!declaration.is_object() || !declaration.contains("name")) continue;
                            const auto value = bound->find(declaration.at("name").get<std::string>());
                            if (value != bound->end()) declaration["default"] = *value;
                        }
                    }
                }
            }
            if (instance.at("parameter_schema").is_null())
                instance["reason"] = "This feature does not declare editable parameters.";
            else if (!source_editable) instance["reason"] = "Fork the engine preset into the project before editing its feature parameters.";
            else instance["editable"] = true;
        } catch (const std::exception &error) {
            instance["reason"] = error.what();
        }
        instances.push_back(std::move(instance));
    }
    };
    append_instances(renderingSectionOrEmpty(root).value("features", Json::array()), "root",
                     dependencies_.source_reference, document_.sourceDigest(), true);
    if (dependencies_.resolve_authoring_context) {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (current.pipeline_preset && current.pipeline_preset_bytes) {
            const auto preset = Json::parse(*current.pipeline_preset_bytes);
            append_instances(preset.at("config").value("features", Json::array()), "preset",
                             current.pipeline_preset->reference, renderConfigSourceDigest(*current.pipeline_preset_bytes),
                             current.pipeline_preset->reference.starts_with("project://"));
        }
    }
    return {{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(document_.sourceDigest())},
            {"features", document_.featureReferences()},
            {"feature_instances", std::move(instances)},
            {"has_uneditable_feature_entries",
             uneditable_entries != 0},
            {"uneditable_feature_entry_count",
             uneditable_entries},
            {"runtime", runtimeJson(dependencies_.runtime_snapshot())}};
}

OrderedJson RenderConfigEditorService::listRenderFeatures(
    const Json &params) const {
    requireOnly(params, {}, "list_render_features");
    auto features = OrderedJson::array();
    for (const auto &entry : feature_catalog_) {
        auto encoded = OrderedJson{
            {"name", entry.name},
            {"reference", entry.reference},
            {"requires_runtime_module", entry.requires_runtime_module},
            {"hot_add_supported", entry.available},
            {"available", entry.available},
            {"source", entry.source},
        };
        if (!entry.available) {
            encoded["unavailable_reason"] =
                entry.unavailable_reason;
        }
        features.push_back(std::move(encoded));
    }
    return {{"features", std::move(features)}};
}

OrderedJson RenderConfigEditorService::materializeRenderFeature(const std::string &base_digest, const Json &operation) {
    constexpr auto method = "materialize render feature";
    requireOnly(operation, {"op", "scope", "index", "destination_reference", "base_feature_digest", "base_preset_digest"}, method);
    const auto scope = operation.value("scope", std::string{"root"});
    const auto destination = requireString(operation, "destination_reference", method);
    const auto feature_digest = requireString(operation, "base_feature_digest", method);
    if ((scope != "root" && scope != "preset") || !operation.contains("index") ||
        !operation.at("index").is_number_integer() || operation.at("index") < 0)
        throw std::invalid_argument("materialize requires root/preset scope and a nonnegative feature index");
    if (!isLowerHexDigest(feature_digest))
        throw std::invalid_argument("base_feature_digest must be a SHA-256 digest");
    if (!dependencies_.resolve_authoring_context || !dependencies_.resolve_document_path || !dependencies_.normalize_reference)
        return rejection("method_unavailable", "Feature materialization requires the project document resolver");
    if (!destination.starts_with("project://") || destination.find('#') != std::string::npos)
        return rejection("invalid_feature_destination", "Choose a new project:// feature file");
    try {
        const auto documents = currentDocumentSet();
        const auto current = dependencies_.resolve_authoring_context(documents);
        auto source = OrderedJson::parse(document_.bytes());
        std::optional<std::string> preset_digest;
        if (scope == "preset") {
            preset_digest = requireString(operation, "base_preset_digest", method);
            if (!isLowerHexDigest(*preset_digest)) throw std::invalid_argument("base_preset_digest must be a SHA-256 digest");
            if (!current.pipeline_preset || !current.pipeline_preset_bytes ||
                !current.pipeline_preset->reference.starts_with("project://"))
                return rejection("project_preset_required", "Fork the engine preset before materializing its features");
            if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != *preset_digest)
                return rejection("external_modification", "The preset changed since it was read");
            source = OrderedJson::parse(*current.pipeline_preset_bytes);
        } else if (operation.contains("base_preset_digest")) {
            throw std::invalid_argument("Root feature materialization does not take base_preset_digest");
        }
        auto &config = scope == "preset" ? source.at("config")
                                         : renderingSectionOf(source);
        auto &features = config.at("features");
        const auto index = operation.at("index").get<std::size_t>();
        if (index >= features.size()) return rejection("feature_not_found", "Feature instance index is out of range");
        const auto &entry = features.at(index);
        const auto reference = entry.is_string() ? entry.get<std::string>() : entry.at("ref").get<std::string>();
        const auto path = dependencies_.resolve_document_path(destination);
        if (std::filesystem::exists(path))
            return rejection("feature_destination_exists", "The destination already exists: " + destination);
        const auto loader = [&](std::string_view ref) {
            std::string bytes;
            if (const auto *candidate = documents.find(dependencies_.normalize_reference(ref))) bytes = candidate->bytes;
            else if (ref.starts_with("engine://")) bytes = engineResourceOrThrow(ref.substr(9));
            else bytes = readFileBytes(dependencies_.resolve_document_path(ref));
            if (ref == reference && renderConfigSourceDigest(bytes) != feature_digest)
                throw std::runtime_error("The feature document changed since it was read");
            return bytes;
        };
        std::optional<Json> materialized;
        std::set<std::string> edited_passes;
        const auto include_overrides = [&](const Json &fragment) {
            if (const auto overrides = fragment.find("pass_overrides"); overrides != fragment.end())
                for (const auto &[name, value] : overrides->items()) edited_passes.insert(name);
        };
        const auto composition = composeRenderFeatureConfig(Json::parse(document_.bytes()), RenderFeatureComposeDependencies{
            .load_feature_json = loader,
            // This authors the currently selected feature. Actual device/module
            // availability is checked by the existing candidate publication path.
            .runtime_shader_compiler_enabled = true,
            .load_pipeline_json = loader,
            .materialized_feature = [&](std::string_view ref, const Json &feature) {
                if (ref != reference) return;
                if (materialized) throw std::runtime_error("Select a uniquely referenced feature instance");
                materialized = feature;
            },
            .applied_integration = [&](std::string_view ref, const Json &fragment) {
                if (ref == reference) include_overrides(fragment);
            },
        });
        if (!materialized) return rejection("feature_not_found", "The selected feature was not composed");
        features[index] = destination;
        for (const auto &pass : composition.pass_provenance)
            if (pass.provider_reference == reference) edited_passes.insert(pass.name);
        include_overrides(*materialized);
        for (const auto &contract : composition.surface_resource_contracts) {
            if (contract.at("provider_ref") != reference) continue;
            for (const auto *field : {"material_consumers", "fullscreen_consumers"})
                for (const auto &consumer : contract.at(field)) edited_passes.insert(consumer.get<std::string>());
        }
        std::map<std::string, std::set<std::string>> released_regions;
        for (const auto &graph : composition.config.at("rendering_passes"))
            for (const auto &pass : graph.at("passes")) {
                if (!edited_passes.contains(pass.at("name").get<std::string>())) continue;
                for (const auto &tag : pass.value("regions", Json::array()))
                    released_regions[graph.at("name").get<std::string>()].insert(tag.get<std::string>());
            }
        const auto release_replacements = [&](OrderedJson &owner_config) {
            bool changed = false;
            if (!owner_config.contains("rendering_passes")) return changed;
            for (auto &graph : owner_config.at("rendering_passes")) {
                const auto regions = released_regions.find(graph.at("name").get<std::string>());
                if (regions == released_regions.end() || !graph.contains("region_replacements")) continue;
                auto &requests = graph.at("region_replacements");
                for (auto request = requests.begin(); request != requests.end();) {
                    if (regions->second.contains(request->at("region").get<std::string>())) {
                        request = requests.erase(request);
                        changed = true;
                    } else ++request;
                }
                if (requests.empty()) graph.erase("region_replacements");
            }
            return changed;
        };
        // Persist removal of the selected regions, so later compiles cannot
        // replace the newly editable nodes with their former fused implementation.
        const bool root_regions_changed =
            scope == "root" && release_replacements(renderingSectionOf(source));
        std::optional<OrderedJson> preset;
        if (scope == "preset") preset = source;
        else if (current.pipeline_preset_bytes) preset = OrderedJson::parse(*current.pipeline_preset_bytes);
        const bool preset_regions_changed = preset && release_replacements(preset->at("config"));
        if (preset_regions_changed && !current.pipeline_preset->reference.starts_with("project://"))
            return rejection("project_preset_required", "Fork the engine preset before releasing its region replacements");
        if (preset_regions_changed && !preset_digest)
            preset_digest = renderConfigSourceDigest(*current.pipeline_preset_bytes);
        std::vector<RenderConfigCandidateDocument> additional{{
            .reference = destination, .normalized_reference = dependencies_.normalize_reference(destination),
            .path = path, .operation = RenderConfigDocumentOperation::create,
            .expected = {RenderConfigDocumentExistence::missing, {}}, .bytes = materialized->dump(2)}};
        if (scope == "preset" || preset_regions_changed) {
            const auto &preset_ref = current.pipeline_preset->reference;
            additional.push_back({.reference = preset_ref, .normalized_reference = dependencies_.normalize_reference(preset_ref),
                .path = dependencies_.resolve_document_path(preset_ref), .operation = RenderConfigDocumentOperation::replace,
                .expected = {RenderConfigDocumentExistence::present, *preset_digest}, .bytes = preset->dump(2)});
        }
        return queueRootEdit(base_digest, "materialize_render_feature",
            [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                if (scope == "root") candidate = candidate.withSectionValue("features", features.dump(2));
                if (root_regions_changed)
                    candidate = candidate.withSectionValue(
                        "rendering_passes",
                        renderingSectionOf(source).at("rendering_passes").dump(2));
                return std::nullopt;
            }, additional, preset_digest);
    } catch (const RenderConfigSectionUnlocated &error) {
        return rejection("render_config_section_unlocated", error.what());
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

OrderedJson RenderConfigEditorService::exportRenderFeature(const Json &params) {
    constexpr auto method = "export_render_feature";
    requireOnly(params, {"scope", "index"}, method);
    const auto scope = params.value("scope", std::string{"root"});
    if ((scope != "root" && scope != "preset") || !params.contains("index") ||
        !params.at("index").is_number_integer() || params.at("index") < 0) {
        throw std::invalid_argument(
            "export_render_feature requires root/preset scope and a nonnegative feature index");
    }
    if (!dependencies_.resolve_document_path) {
        return rejection("method_unavailable",
                         "Feature export requires the project document resolver");
    }
    try {
        Json features = Json::array();
        if (scope == "preset") {
            if (!dependencies_.resolve_authoring_context) {
                return rejection("method_unavailable",
                                 "Preset feature export requires the authoring context resolver");
            }
            const auto current =
                dependencies_.resolve_authoring_context(currentDocumentSet());
            if (!current.pipeline_preset_bytes) {
                return rejection("pipeline_preset_required",
                                 "The current rendering config does not select a preset");
            }
            features = Json::parse(*current.pipeline_preset_bytes)
                           .at("config")
                           .value("features", Json::array());
        } else {
            const auto root = Json::parse(document_.bytes());
            features =
                renderingSectionOrEmpty(root).value("features", Json::array());
        }
        const auto index = params.at("index").get<std::size_t>();
        if (index >= features.size())
            return rejection("feature_not_found", "Feature instance index is out of range");
        const auto &entry = features.at(index);
        const auto reference = entry.is_string()
                                   ? entry.get<std::string>()
                                   : entry.at("ref").get<std::string>();
        if (reference.starts_with("engine://")) {
            return rejection(
                "render_component_engine_reference",
                "Engine features are already shared; derive a project copy first",
                {{"reference", reference}});
        }
        const auto component = buildRenderComponent(
            readFileBytes(dependencies_.resolve_document_path(reference)),
            reference, dependencies_.project_root.filename().string());
        const auto destination_reference =
            "user://components/" + component.name;
        writeRenderComponent(
            dependencies_.resolve_document_path(destination_reference),
            component);
        return {{"status", "exported"},
                {"component_reference", destination_reference},
                {"name", component.name},
                {"feature_digest", digestJson(component.feature_digest)},
                {"requires",
                 {{"render_targets", component.required_render_targets}}},
                {"source_reference", reference}};
    } catch (const RenderComponentError &error) {
        return rejection(error.code(), error.what());
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

OrderedJson RenderConfigEditorService::importRenderFeature(const Json &params) {
    constexpr auto method = "import_render_feature";
    requireOnly(params, {"base_source_digest", "source_dir", "destination_reference"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto source_dir = requireString(params, "source_dir", method);
    const auto destination = requireString(params, "destination_reference", method);
    if (!dependencies_.resolve_document_path || !dependencies_.normalize_reference) {
        return rejection("method_unavailable",
                         "Feature import requires the project document resolver");
    }
    if (!destination.starts_with("project://") ||
        destination.find('#') != std::string::npos) {
        return rejection("invalid_feature_destination",
                         "Choose a new project:// feature file");
    }
    try {
        const std::filesystem::path source{source_dir};
        if (!source.is_absolute()) {
            return rejection("render_component_invalid",
                             "source_dir must be an absolute path to a component directory");
        }
        if (isWithinDirectory(dependencies_.project_root, source)) {
            return rejection(
                "render_component_source_inside_project",
                "A document inside the project is referenced with project://, not imported");
        }
        const auto component = readRenderComponent(source);
        // The host contract: every render target the component reads or writes
        // without declaring it has to be declared here.  `swapchain` is the
        // engine's own output and is always present.
        std::set<std::string, std::less<>> declared{"swapchain"};
        const auto collect = [&declared](const Json &config) {
            for (const auto &target : config.value("render_targets", Json::array())) {
                if (target.is_string()) declared.insert(target.get<std::string>());
                else if (target.is_object() && target.contains("name") &&
                         target.at("name").is_string())
                    declared.insert(target.at("name").get<std::string>());
            }
        };
        const auto root = Json::parse(document_.bytes());
        collect(renderingSectionOrEmpty(root));
        if (dependencies_.resolve_authoring_context) {
            const auto current =
                dependencies_.resolve_authoring_context(currentDocumentSet());
            if (current.pipeline_preset_bytes)
                collect(Json::parse(*current.pipeline_preset_bytes).at("config"));
        }
        auto missing = Json::array();
        std::string missing_names;
        for (const auto &name : component.required_render_targets) {
            if (declared.contains(name)) continue;
            if (!missing_names.empty()) missing_names += ", ";
            missing_names += name;
            missing.push_back(name);
        }
        if (!missing.empty()) {
            return rejection(
                "render_component_requires_missing",
                "This project does not declare the render targets the component reads: " +
                    missing_names,
                {{"render_targets", missing}});
        }
        const auto path = dependencies_.resolve_document_path(destination);
        if (std::filesystem::exists(path)) {
            return rejection("feature_destination_exists",
                             "The destination already exists: " + destination);
        }
        const std::array documents{RenderConfigCandidateDocument{
            .reference = destination,
            .normalized_reference = dependencies_.normalize_reference(destination),
            .path = path,
            .operation = RenderConfigDocumentOperation::create,
            .expected = {RenderConfigDocumentExistence::missing, {}},
            .bytes = component.feature_bytes}};
        auto result = queueRootEdit(
            base_digest, method,
            [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                if (!candidate.hasFeaturesArray())
                    candidate = AuthoredRenderConfigDocument::initialize(candidate.bytes());
                candidate = candidate.withFeatureAdded(destination);
                return std::nullopt;
            },
            documents);
        if (result.at("status") == "accepted") {
            result["feature_reference"] = destination;
            result["name"] = component.name;
        }
        return result;
    } catch (const RenderComponentError &error) {
        return rejection(error.code(), error.what());
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

namespace {

// Every string value in the document, wherever it sits.  A reference into a
// package can live in `features`, in a preset pass, in a shader asset binding
// or in an override, so the scan is structural and not field-by-field.
void collectPackageReferences(const Json &value, const std::string &prefix,
                              std::set<std::string> &found) {
    if (value.is_string()) {
        const auto &text = value.get_ref<const std::string &>();
        if (text.starts_with(prefix)) found.insert(text);
        return;
    }
    if (value.is_object() || value.is_array()) {
        for (const auto &child : value)
            collectPackageReferences(child, prefix, found);
    }
}

} // namespace

OrderedJson RenderConfigEditorService::installPackage(const Json &params) {
    constexpr auto method = "install_package";
    requireOnly(params, {"source_dir"}, method);
    const auto source_dir = requireString(params, "source_dir", method);
    if (dependencies_.project_root.empty()) {
        return rejection("method_unavailable",
                         "Package install requires a project root");
    }
    try {
        const std::filesystem::path source{source_dir};
        if (!source.is_absolute()) {
            return rejection(
                "package_manifest_invalid",
                "source_dir must be an absolute path to a package directory");
        }
        if (isWithinDirectory(dependencies_.project_root, source)) {
            return rejection(
                "package_source_inside_project",
                "A package that already lives in the project is referenced "
                "with project://, not installed");
        }
        const auto result =
            Pelican::installPackage(source, dependencies_.project_root);
        // The catalog is rebuilt so the new documents are visible without
        // reopening the project.
        if (dependencies_.feature_catalog)
            feature_catalog_ = dependencies_.feature_catalog();
        return {{"status", "installed"},
                {"namespace", result.manifest.name_space},
                {"package_version", result.manifest.package_version},
                {"sha256", result.sha256},
                {"feature_references", result.feature_references},
                {"feature_names", result.feature_names}};
    } catch (const PackageError &error) {
        OrderedJson details;
        if (!error.names().empty()) details["names"] = error.names();
        return rejection(error.code(), error.what(), std::move(details));
    } catch (const std::exception &error) {
        return rejection("package_manifest_invalid", error.what());
    }
}

OrderedJson RenderConfigEditorService::uninstallPackage(const Json &params) {
    constexpr auto method = "uninstall_package";
    requireOnly(params, {"namespace"}, method);
    const auto name_space = requireString(params, "namespace", method);
    if (dependencies_.project_root.empty()) {
        return rejection("method_unavailable",
                         "Package uninstall requires a project root");
    }
    try {
        const auto prefix = packageReferencePrefix(name_space);
        std::set<std::string> in_use;
        collectPackageReferences(Json::parse(document_.bytes()), prefix,
                                 in_use);
        if (dependencies_.resolve_authoring_context) {
            try {
                const auto current =
                    dependencies_.resolve_authoring_context(
                        currentDocumentSet());
                if (current.pipeline_preset_bytes) {
                    collectPackageReferences(
                        Json::parse(*current.pipeline_preset_bytes), prefix,
                        in_use);
                }
            } catch (const std::exception &) {
                // A config that does not compose still has to be able to drop
                // the package that broke it: the lexical scan above already
                // covered the root document.
            }
        }
        Pelican::uninstallPackage(
            dependencies_.project_root, name_space,
            std::vector<std::string>{in_use.begin(), in_use.end()});
        if (dependencies_.feature_catalog)
            feature_catalog_ = dependencies_.feature_catalog();
        return {{"status", "uninstalled"}, {"namespace", name_space}};
    } catch (const PackageError &error) {
        OrderedJson details;
        if (!error.names().empty()) details["references"] = error.names();
        return rejection(error.code(), error.what(), std::move(details));
    } catch (const std::exception &error) {
        return rejection("package_manifest_invalid", error.what());
    }
}

OrderedJson RenderConfigEditorService::editRenderFeatures(
    const Json &params) {
    requireOnly(params, {"base_source_digest", "operations"},
                "edit_render_features");
    const auto base_digest = requireString(
        params, "base_source_digest", "edit_render_features");
    const auto operations = params.find("operations");
    if (operations == params.end() || !operations->is_array() ||
        operations->size() > 1) {
        throw std::invalid_argument(
            "edit_render_features operations must be an array containing at most one element in v1");
    }

    const auto patch_parameters = [&](Json &features, const Json &operation) -> std::optional<OrderedJson> {
        if (!operation.contains("index") || !operation.at("index").is_number_integer() || operation.at("index") < 0 ||
            !operation.contains("parameters") || !operation.at("parameters").is_object())
            return rejection("invalid_feature_parameters", "set_parameters requires a nonnegative index and parameters object");
        const auto index = operation.at("index").get<std::size_t>();
        if (!features.is_array() || index >= features.size())
            return rejection("invalid_feature_parameters", "Feature instance index is out of range");
        auto &entry = features.at(index);
        const auto &parameters = operation.at("parameters");
        if (entry.is_string()) {
            if (!parameters.empty()) entry = {{"ref", entry.get<std::string>()}, {"parameters", parameters}};
        } else if (entry.is_object() && entry.contains("ref") && entry.at("ref").is_string()) {
            entry["parameters"] = parameters;
        } else return rejection("invalid_feature_parameters", "The selected feature instance has no editable reference");
        return std::nullopt;
    };
    if (!operations->empty() && operations->front().value("op", std::string{}) == "set_parameters" &&
        operations->front().value("scope", std::string{"root"}) == "preset") {
        const auto &operation = operations->front();
        requireOnly(operation, {"op", "scope", "index", "parameters", "base_preset_digest"}, "edit_render_features operation");
        const auto digest = requireString(operation, "base_preset_digest", "set_parameters");
        if (!isLowerHexDigest(digest)) throw std::invalid_argument("base_preset_digest must be a SHA-256 digest");
        try {
            if (!dependencies_.resolve_authoring_context || !dependencies_.resolve_document_path || !dependencies_.normalize_reference)
                return rejection("project_preset_required", "Project preset document access is unavailable");
            const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
            if (!current.pipeline_preset || !current.pipeline_preset_bytes ||
                !current.pipeline_preset->reference.starts_with("project://"))
                return rejection("project_preset_required", "Fork the selected preset into the project before editing its feature parameters");
            if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != digest)
                return rejection("external_modification", "The preset changed since its feature parameters were read");
            auto preset = OrderedJson::parse(*current.pipeline_preset_bytes);
            auto features = Json(preset.at("config").value("features", OrderedJson::array()));
            if (const auto rejected = patch_parameters(features, operation)) return *rejected;
            preset["config"]["features"] = features;
            const auto &reference = current.pipeline_preset->reference;
            const std::array documents{RenderConfigCandidateDocument{
                .reference = reference, .normalized_reference = dependencies_.normalize_reference(reference),
                .path = dependencies_.resolve_document_path(reference), .operation = RenderConfigDocumentOperation::replace,
                .expected = {RenderConfigDocumentExistence::present, digest}, .bytes = preset.dump(2)}};
            return queueRootEdit(base_digest, "edit_render_features",
                [](AuthoredRenderConfigDocument &) -> std::optional<OrderedJson> { return std::nullopt; }, documents, digest);
        } catch (const std::exception &error) {
            return rejection("invalid_feature_parameters", error.what());
        }
    }

    if (!operations->empty() && operations->front().value("op", std::string{}) == "materialize")
        return materializeRenderFeature(base_digest, operations->front());
    return queueRootEdit(base_digest, "edit_render_features",
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            candidate = document_.hasFeaturesArray()
                                 ? document_
                                 : AuthoredRenderConfigDocument::initialize(
                                       document_.bytes());
            if (!operations->empty()) {
                const auto &operation = operations->front();
                const auto op = requireString(operation, "op",
                                              "edit_render_features operation");
                if (op == "set_parameters") {
                    requireOnly(operation, {"op", "scope", "index", "parameters"}, "edit_render_features operation");
                    if (operation.value("scope", std::string{"root"}) != "root")
                        return rejection("invalid_feature_parameters", "Feature parameter scope must be root or preset");
                    const auto parsed = Json::parse(candidate.bytes());
                    auto features = renderingSectionOrEmpty(parsed).value("features", Json::array());
                    if (const auto rejected = patch_parameters(features, operation)) return rejected;
                    candidate = candidate.withSectionValue("features", features.dump(2));
                    return std::nullopt;
                }
                requireOnly(operation, {"op", "feature"}, "edit_render_features operation");
                const auto feature = requireString(
                    operation, "feature", "edit_render_features operation");
                if (op == "add") {
                    const auto *catalog = findCatalogReference(feature);
                    if (catalog == nullptr) {
                        return rejection(
                            "unknown_render_feature",
                            "render feature is not present in the engine catalog: " +
                                feature,
                            {{"feature", feature}});
                    }
                    if (!candidate.containsFeature(feature) &&
                        !catalog->available) {
                        return rejection(
                            catalog->requires_runtime_module
                                ? "restart_required_feature"
                                : "render_feature_unavailable",
                            catalog->unavailable_reason,
                            {{"feature", catalog->name},
                             {"reference", catalog->reference}});
                    }
                    candidate = candidate.withFeatureAdded(feature);
                } else if (op == "remove") {
                    try {
                        candidate = candidate.withFeatureRemoved(feature);
                    } catch (const std::invalid_argument &error) {
                        return rejection("render_feature_not_enabled", error.what(),
                                         {{"feature", feature}});
                    }
                } else {
                    throw std::invalid_argument(
                        "edit_render_features operation op must be 'add', 'remove' or 'set_parameters'");
                }
            }

            return std::nullopt;
        });
}

// The authoring precondition preamble shared by the root-edit queue and
// add_authored_pass: gate -> base source digest CAS -> re-read-from-disk CAS,
// in that order.  Returns the rejection the RPC answers with, or nullopt when
// the edit may proceed.  The gate snapshot stays with the caller because the
// accepted ticket records its epochs.
std::optional<OrderedJson>
RenderConfigEditorService::checkAuthoringPreconditions(
    const GateSnapshot &gate, const std::string &base_digest,
    std::string_view method, std::string_view gate_closed_message) const {
    if (!gate.can_edit) {
        return rejection(
            "gate_closed", std::string{gate_closed_message},
            {{"method", std::string{method}},
             {"reason", primaryReason(gate.reasons)},
             {"gate_epoch", gate.epoch}});
    }
    if (base_digest != document_.sourceDigest()) {
        return rejection(
            "external_modification",
            "render config base digest no longer matches the engine-owned document",
            {{"expected_source_digest", document_.sourceDigest()},
             {"provided_source_digest", base_digest}});
    }
    try {
        const auto disk_digest =
            renderConfigSourceDigest(readFileBytes(dependencies_.source_path));
        if (disk_digest != document_.sourceDigest()) {
            return rejection(
                "external_modification",
                "render config source changed outside the editor",
                {{"expected_source_digest", document_.sourceDigest()},
                 {"actual_source_digest", disk_digest}});
        }
    } catch (const std::exception &error) {
        return rejection("external_modification", error.what());
    }
    return std::nullopt;
}

OrderedJson RenderConfigEditorService::queueRootEdit(
    const std::string &base_digest, const std::string &operation,
    const std::function<std::optional<OrderedJson>(AuthoredRenderConfigDocument &)> &transform,
    std::span<const RenderConfigCandidateDocument> additional_documents,
    std::optional<std::string> base_preset_digest, bool source_only) {
    if (!isLowerHexDigest(base_digest))
        throw std::invalid_argument(operation + " base_source_digest must be 64 lowercase hex characters");
    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(
            gate, base_digest, operation,
            "render config editing is currently disabled")) {
        return *rejected;
    }

    auto candidate = document_;
    try {
        if (auto rejected = transform(candidate)) return *rejected;
    } catch (const RenderConfigSectionUnlocated &error) {
        return rejection("render_config_section_unlocated", error.what(),
                         {{"method", operation}});
    }
    const auto ticket_id =
        "render-config-edit-" + std::to_string(next_ticket_++);
    auto candidate_documents = currentDocumentSet().documents();
    const auto root_document = std::find_if(
        candidate_documents.begin(), candidate_documents.end(),
        [&](const auto &entry) {
            return entry.normalized_reference ==
                   source_normalized_reference_;
        });
    if (root_document == candidate_documents.end()) {
        throw std::logic_error(
            "render config edit candidate lost its root document");
    }
    root_document->bytes = candidate.bytes();
    for (const auto &extra : additional_documents) {
        if (extra.normalized_reference == source_normalized_reference_)
            return rejection("invalid_render_document", "Additional documents cannot replace the root edit");
        const auto existing = std::find_if(candidate_documents.begin(), candidate_documents.end(), [&](const auto &entry) {
            return entry.normalized_reference == extra.normalized_reference;
        });
        if (existing == candidate_documents.end()) {
            candidate_documents.insert(candidate_documents.begin(), extra);
        } else {
            if (existing->expected.digest != extra.expected.digest)
                return rejection("external_modification", "The managed document changed outside the editor: " + extra.reference);
            *existing = extra;
        }
    }
    RenderConfigCandidateDocumentSet candidate_set{
        source_normalized_reference_, std::move(candidate_documents)};
    // G-6: an edit the engine will compile is composed by every variant it
    // registered first. An edit that only moves bytes the engine never reads
    // (source_only) has nothing to compose.
    if (!source_only) {
        if (auto refused = preflightGraphVariants(
                candidate_set, {{"participant", "pelican.render_pipeline"},
                                {"method", operation}})) {
            return *refused;
        }
    }
    pending_.push_back(Ticket{
        .id = ticket_id,
        .accepted_gate_epoch = gate.epoch,
        .base_source_digest = base_digest,
        .candidate = std::move(candidate),
        .candidate_documents = std::move(candidate_set),
        .operation = operation,
        .base_preset_digest = std::move(base_preset_digest),
        .edits_additional_documents = !additional_documents.empty(),
        .source_only = source_only,
    });
    auto accepted = OrderedJson{
        {"ticket", ticket_id},
        {"status", "accepted"},
        {"base_source_digest", digestJson(base_digest)},
    };
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return accepted;
}

OrderedJson RenderConfigEditorService::getRayCallableRecords(const Json &params) const {
    requireOnly(params, {}, "get_ray_callable_records");
    const auto root = Json::parse(document_.bytes());
    const auto config = dependencies_.resolve_authoring_context
        ? dependencies_.resolve_authoring_context(currentDocumentSet()).config
        : renderingSectionOrEmpty(root);
    const auto overrides = renderingSectionOrEmpty(root).value("ray_task_callable_records", Json::object());
    auto tasks = Json::array();
    for (const auto &task : config.value("compute_tasks", Json::array())) {
        if (!task.contains("ray_tracing")) continue;
        const auto &ray = task.at("ray_tracing");
        if (!ray.contains("callable_record")) continue;
        const auto name = task.at("name").get<std::string>();
        const auto &declared_callables = ray.at("callable");
        const auto callables = declared_callables.is_array() ? declared_callables : Json::array({declared_callables});
        tasks.push_back({{"name", name}, {"schema", ray.at("callable_record")},
            {"callables", callables}, {"authored_records", ray.at("callable_records")},
            {"records", overrides.value(name, ray.at("callable_records"))}});
    }
    return {{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(document_.sourceDigest())}, {"tasks", tasks}};
}

OrderedJson RenderConfigEditorService::editRayCallableRecords(const Json &params) {
    requireOnly(params, {"base_source_digest", "task_records"}, "edit_ray_callable_records");
    const auto digest = requireString(params, "base_source_digest", "edit_ray_callable_records");
    return queueRootEdit(digest, "edit_ray_callable_records",
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            if (!params.contains("task_records") || !params.at("task_records").is_object())
                return rejection("invalid_callable_records", "task_records must be an object");
            const auto context = getRayCallableRecords(Json::object());
            const auto &tasks = context.at("tasks");
            for (const auto &[name, records] : params.at("task_records").items()) {
                const auto task = std::find_if(tasks.begin(), tasks.end(), [&](const auto &entry) {
                    return entry.at("name") == name;
                });
                if (task == tasks.end())
                    return rejection("invalid_callable_records", "Unknown callable record task: " + name);
                try {
                    const auto layout = parseRayCallableRecordLayout(task->at("schema"), name);
                    (void)encodeRayCallableRecords(records, layout, task->at("callables").size(), name);
                } catch (const std::exception &error) {
                    return rejection("invalid_callable_records", error.what());
                }
            }
            candidate = candidate.withSectionValue("ray_task_callable_records", params.at("task_records").dump(2));
            return std::nullopt;
        });
}

OrderedJson RenderConfigEditorService::getSceneAccelerationStructures(const Json &params) const {
    requireOnly(params, {}, "get_scene_acceleration_structures");
    const auto root = Json::parse(document_.bytes());
    Json connections = Json::array();
    const auto config = dependencies_.resolve_authoring_context
        ? dependencies_.resolve_authoring_context(currentDocumentSet()).config
        : renderingSectionOrEmpty(root);
    const auto overrides = renderingSectionOrEmpty(root).value("ray_task_acceleration_structures", Json::object());
    for (const auto &task : config.value("compute_tasks", Json::array())) {
        if (!task.contains("ray_tracing") && !task.contains("ray_query")) continue;
        const auto name = task.at("name").get<std::string>();
        const auto &ray = task.at(task.contains("ray_tracing") ? "ray_tracing" : "ray_query");
        const auto authored = ray.value("acceleration_structure", std::string{"scene"});
        connections.push_back({{"name", name}, {"authored_acceleration_structure", authored},
            {"acceleration_structure", overrides.value(name, authored)}});
    }
    return {{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(document_.sourceDigest())},
            {"task_connections", connections},
            {"triangle_geometry", geometrySpaceDocument(config, dependencies_).info},
            {"procedural_geometry", geometrySpaceDocument(config, dependencies_, true).info},
            {"definitions", renderingSectionOrEmpty(root).value("scene_acceleration_structures", Json::array())}};
}

OrderedJson RenderConfigEditorService::editSceneAccelerationStructures(const Json &params) {
    requireOnly(params, {"base_source_digest", "definitions", "task_connections", "triangle_space", "procedural_space", "base_layout_digest"}, "edit_scene_acceleration_structures");
    const auto base_digest = requireString(params, "base_source_digest", "edit_scene_acceleration_structures");
    if (!params.contains("definitions"))
        throw std::invalid_argument("edit_scene_acceleration_structures requires definitions");
    std::vector<RenderConfigCandidateDocument> layout_documents;
    const bool edits_space = params.contains("triangle_space") || params.contains("procedural_space");
    const auto error_code = params.contains("procedural_space") ? "invalid_procedural_space" : "invalid_triangle_space";
    if (edits_space != params.contains("base_layout_digest"))
        return rejection(error_code, "Coordinate space edits and base_layout_digest must be supplied together");
    if (edits_space) {
        try {
            const auto digest = requireString(params, "base_layout_digest", "edit_scene_acceleration_structures");
            if (!isLowerHexDigest(digest))
                return rejection(error_code, "base_layout_digest must be a SHA-256 digest");
            const auto parsed = Json::parse(document_.bytes());
            const auto config = dependencies_.resolve_authoring_context
                ? dependencies_.resolve_authoring_context(currentDocumentSet()).config
                : renderingSectionOrEmpty(parsed);
            auto layout = geometrySpaceDocument(config, dependencies_, params.contains("procedural_space"));
            if (!layout.info.at("editable").get<bool>())
                return rejection(params.contains("procedural_space") ? "procedural_space_not_editable" : "triangle_space_not_editable", layout.info.value("reason", std::string{"Ray group layout is not editable"}));
            if (layout.info.at("source_digest").at("hex") != digest)
                return rejection("external_modification", "The ray group layout changed since it was read");
            auto document = OrderedJson::parse(layout.bytes);
            if (!document.contains("acceleration_structure"))
                document["acceleration_structure"] = {{"defaults", OrderedJson::object()}};
            for (const auto *field : {"triangle_space", "procedural_space"}) {
                if (!params.contains(field)) continue;
                const auto space = requireString(params, field, "edit_scene_acceleration_structures");
                if (space != "geometry" && space != "world")
                    return rejection(error_code, "Coordinate space must be geometry or world");
                document["acceleration_structure"][field] = space;
            }
            const auto bytes = document.dump(2);
            const auto reference = layout.info.at("source_reference").get<std::string>();
            (void)parseRayGroupLayout(bytes, reference);
            layout_documents.push_back({
                .reference = reference, .normalized_reference = dependencies_.normalize_reference(reference),
                .path = layout.path, .operation = RenderConfigDocumentOperation::replace,
                .expected = {RenderConfigDocumentExistence::present, digest}, .bytes = bytes});
        } catch (const std::exception &error) {
            return rejection(error_code, error.what());
        }
    }
    return queueRootEdit(base_digest, "edit_scene_acceleration_structures",
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            ResolvedRenderPipeline validation;
            validation.normalized_config["scene_acceleration_structures"] = params.at("definitions");
            try {
                (void)compileRenderPipeline(validation);
            } catch (const std::exception &error) {
                return rejection("invalid_scene_acceleration_structures", error.what());
            }
            if (params.contains("task_connections")) {
                const auto &connections = params.at("task_connections");
                if (!connections.is_object())
                    return rejection("invalid_task_connections", "task_connections must be an object");
                const auto context = getSceneAccelerationStructures(Json::object());
                for (const auto &[name, selected] : connections.items()) {
                    if (!selected.is_string() || selected.get_ref<const std::string &>().empty())
                        return rejection("invalid_task_connections", "AS names must be nonempty strings");
                    const auto &tasks = context.at("task_connections");
                    if (std::none_of(tasks.begin(), tasks.end(), [&](const auto &task) { return task.at("name") == name; }))
                        return rejection("invalid_task_connections", "Unknown ray task: " + name);
                    const auto target = selected.get<std::string>();
                    if (target != "scene" && std::none_of(params.at("definitions").begin(), params.at("definitions").end(),
                        [&](const auto &definition) { return definition.at("name") == target; }))
                        return rejection("invalid_task_connections", "Unknown scene acceleration structure: " + target);
                }
                candidate = candidate.withSectionValue("ray_task_acceleration_structures", connections.dump(2));
            }
            candidate = candidate.withSectionValue("scene_acceleration_structures", params.at("definitions").dump(2));
            return std::nullopt;
        }, layout_documents);
}

OrderedJson RenderConfigEditorService::getRenderAuthoringContext(
    const Json &params) const {
    requireOnly(params, {}, "get_render_authoring_context");
    if (!dependencies_.resolve_authoring_context) {
        throw std::logic_error(
            "render authoring context resolver is unavailable");
    }
    const auto resolved =
        dependencies_.resolve_authoring_context(currentDocumentSet());
    const auto runtime = dependencies_.runtime_snapshot();

    std::optional<OrderedJson> editable_preset;
    if (resolved.pipeline_preset && resolved.pipeline_preset_bytes &&
        resolved.pipeline_preset->reference.starts_with("project://")) {
        editable_preset = OrderedJson::parse(*resolved.pipeline_preset_bytes);
    }
    // F-6: a direct root edits the passes it declares itself; a preset root
    // never does (its passes are the preset's).
    std::optional<OrderedJson> editable_root;
    if (!resolved.pipeline_preset) {
        try {
            editable_root = OrderedJson::parse(document_.bytes());
        } catch (const std::exception &) {
            editable_root.reset();
        }
    }

    std::unordered_map<std::string, const RenderPassProvenance *>
        provenance;
    for (const auto &entry : resolved.pass_provenance) {
        provenance.insert_or_assign(entry.name, &entry);
    }

    auto graphs = OrderedJson::array();
    const auto declarations = resolved.config.find("rendering_passes");
    if (declarations == resolved.config.end() ||
        !declarations->is_array()) {
        throw std::runtime_error(
            "resolved render authoring context requires rendering_passes[]");
    }
    for (const auto &graph : *declarations) {
        if (!graph.is_object() || !graph.contains("name") ||
            !graph.at("name").is_string() || !graph.contains("passes") ||
            !graph.at("passes").is_array()) {
            throw std::runtime_error(
                "resolved render authoring graph requires name and passes[]");
        }
        auto passes = OrderedJson::array();
        const auto &pass_declarations = graph.at("passes");
        for (std::size_t index = 0; index < pass_declarations.size();
             ++index) {
            const auto &pass = pass_declarations.at(index);
            if (!pass.is_object() || !pass.contains("name") ||
                !pass.at("name").is_string()) {
                throw std::runtime_error(
                    "resolved render authoring pass requires a string name");
            }
            const auto name = pass.at("name").get<std::string>();
            OrderedJson encoded{
                {"name", name},
                {"declaration_index", index},
                {"declaration", pass},
            };
            auto *preset_pass = editable_preset
                ? findPresetEditablePass(*editable_preset, graph.at("name").get<std::string>(), name)
                : nullptr;
            encoded["editable_preset_pass"] = preset_pass != nullptr;
            if (preset_pass) encoded["preset_declaration"] = *preset_pass;
            auto *root_pass = editable_root
                ? findRootEditablePass(*editable_root, graph.at("name").get<std::string>(), name)
                : nullptr;
            encoded["editable_root_pass"] = root_pass != nullptr;
            if (root_pass) encoded["root_declaration"] = *root_pass;
            const auto source = provenance.find(name);
            encoded["editable_feature_pass"] = false;
            if (source != provenance.end() && source->second->source == RenderPipelineProvenanceSource::feature) {
                try {
                    const auto &reference = source->second->provider_reference;
                    if (reference.empty() || reference.starts_with("engine://"))
                        throw std::runtime_error("This pass belongs to a shared engine feature. Copy it into the project before editing.");
                    if (!dependencies_.resolve_document_path)
                        throw std::runtime_error("Project feature document access is unavailable.");
                    const auto bytes = readFileBytes(dependencies_.resolve_document_path(reference));
                    auto feature = OrderedJson::parse(bytes);
                    if (!source->second->authored_name.empty() && source->second->authored_name != name)
                        throw std::runtime_error("Generated feature variants must be edited in their source document.");
                    const auto *declaration = findFeatureEditablePass(feature,
                        source->second->authored_name.empty() ? name : source->second->authored_name,
                        source->second->provider_feature);
                    if (!declaration) throw std::runtime_error("This node is generated and has no editable pass declaration in its feature.");
                    encoded["editable_feature_pass"] = true;
                    encoded["feature_declaration"] = *declaration;
                    encoded["feature_source_reference"] = reference;
                    encoded["feature_source_digest"] = digestJson(renderConfigSourceDigest(bytes));
                } catch (const std::exception &error) {
                    encoded["feature_edit_reason"] = error.what();
                }
            }
            if (source != provenance.end()) {
                encoded["provenance"] = {
                    {"source",
                     provenanceSourceName(source->second->source)},
                    {"provider_feature",
                     source->second->provider_feature},
                    {"provider_reference",
                     source->second->provider_reference},
                };
                if (!source->second->provider_reference.empty() &&
                    dependencies_.normalize_reference) {
                    try {
                        const auto key = dependencies_.normalize_reference(
                            source->second->provider_reference);
                        encoded["managed"] =
                            managed_documents_.contains(key);
                    } catch (const std::exception &) {
                        encoded["managed"] = false;
                    }
                }
            }
            passes.push_back(std::move(encoded));
        }

        auto anchors = OrderedJson::array();
        if (pass_declarations.empty()) {
            anchors.push_back({{"position", 0}, {"insert", "begin"}});
        } else {
            anchors.push_back(
                {{"position", 0},
                 {"insert", "before:" +
                                pass_declarations.front()
                                    .at("name")
                                    .get<std::string>()}});
            for (std::size_t position = 1;
                 position <= pass_declarations.size(); ++position) {
                anchors.push_back(
                    {{"position", position},
                     {"insert", "after:" +
                                    pass_declarations.at(position - 1)
                                        .at("name")
                                        .get<std::string>()}});
            }
        }
        graphs.push_back({
            {"name", graph.at("name")},
            {"passes", std::move(passes)},
            {"region_replacements", graph.value("region_replacements", Json::array())},
            {"anchor_candidates", std::move(anchors)},
        });
    }

    // F-1: the render target fragment is managed, but it holds no pass. It is
    // reported below as render_targets[], and kept off this list so nothing
    // offers to remove it as if it were an authored pass.
    std::optional<std::string> target_fragment_key;
    try {
        if (dependencies_.normalize_reference)
            target_fragment_key = dependencies_.normalize_reference(
                renderTargetFragmentReference());
    } catch (const std::exception &) {
    }
    auto fragments = OrderedJson::array();
    for (const auto &[key, managed] : managed_documents_) {
        if (target_fragment_key && key == *target_fragment_key) continue;
        auto pass_names = OrderedJson::array();
        const auto feature = Json::parse(managed.bytes);
        if (const auto entries = feature.find("passes");
            entries != feature.end() && entries->is_array()) {
            for (const auto &entry : *entries) {
                if (entry.is_object() && entry.contains("pass") &&
                    entry.at("pass").is_object() &&
                    entry.at("pass").contains("name") &&
                    entry.at("pass").at("name").is_string()) {
                    pass_names.push_back(
                        entry.at("pass").at("name"));
                }
            }
        }
        fragments.push_back({
            {"reference", managed.reference},
            {"digest", digestJson(managed.digest)},
            {"pass_names", std::move(pass_names)},
        });
    }

    OrderedJson result{
        {"source_reference", dependencies_.source_reference},
        {"source_digest", digestJson(document_.sourceDigest())},
        {"published_generation", runtime.published_generation},
        // G-1: what the running pipeline enabled. A scene-class target is
        // R16G16B16A16_SFLOAT only with "hdr" among them, which decides the
        // formats a new G-buffer's targets are made with.
        {"enabled_feature_names", runtime.enabled_feature_names},
        {"config_kind",
         resolved.pipeline_preset ? "preset" : "direct"},
        {"graphs", std::move(graphs)},
        {"managed_fragments", std::move(fragments)},
    };

    // F-1: every composed render target, where it was declared, and whether
    // edit_render_targets may change it. The editable ones come first, in the
    // fragment's own order, then the rest in composed order.
    const auto managed_targets =
        target_fragment_key ? managed_documents_.find(*target_fragment_key)
                            : managed_documents_.end();
    OrderedJson fragment_targets = OrderedJson::array();
    if (managed_targets != managed_documents_.end()) {
        try {
            fragment_targets = fragmentRenderTargets(managed_targets->second.bytes);
        } catch (const std::exception &) {
            fragment_targets = OrderedJson::array();
        }
    }
    const auto origins =
        renderTargetOrigins(resolved, document_.bytes(), fragment_targets);
    auto editable_targets = OrderedJson::array();
    auto other_targets = OrderedJson::array();
    for (const auto &declaration :
         resolved.config.value("render_targets", Json::array())) {
        if (!declaration.is_object() || !declaration.contains("name") ||
            !declaration.at("name").is_string())
            continue;
        const auto name = declaration.at("name").get<std::string>();
        const auto source = origins.source(name);
        OrderedJson entry{{"name", name},
                          {"declaration", declaration},
                          {"editable", source == "authoring"},
                          {"source", source}};
        if (source == "authoring") {
            const auto authored = std::find_if(
                fragment_targets.begin(), fragment_targets.end(),
                [&](const auto &target) {
                    return target.is_object() &&
                           target.value("name", std::string{}) == name;
                });
            if (authored != fragment_targets.end())
                entry["authored_declaration"] = *authored;
            editable_targets.push_back(std::move(entry));
        } else {
            other_targets.push_back(std::move(entry));
        }
    }
    for (auto &entry : other_targets) editable_targets.push_back(std::move(entry));
    result["render_targets"] = std::move(editable_targets);
    result["render_target_fragment"] = renderTargetFragmentReference();
    result["render_target_vocabulary"] = {
        {"formats", imageFormatNames()},
        {"usage", imageUsageFlagNames()},
        {"format_classes", OrderedJson::array({"scene", "display", "data", "explicit"})},
    };
    if (resolved.pipeline_preset) {
        result["preset_material_routing"] = resolved.pipeline_preset_bytes
            ? Json::parse(*resolved.pipeline_preset_bytes).at("config").value("material_routing", Json(nullptr))
            : Json(nullptr);
        result["pipeline_preset"] = {
            {"reference", resolved.pipeline_preset->reference},
            {"name", resolved.pipeline_preset->name},
            {"version", resolved.pipeline_preset->version},
        };
        if (resolved.pipeline_preset_bytes)
            result["pipeline_preset"]["source_digest"] = digestJson(renderConfigSourceDigest(*resolved.pipeline_preset_bytes));
    }
    return result;
}

OrderedJson RenderConfigEditorService::forkRenderPipeline(const Json &params) {
    requireOnly(params, {"base_source_digest", "base_preset_digest", "destination_reference"},
        "fork_render_pipeline");
    const auto base_digest = requireString(params, "base_source_digest", "fork_render_pipeline");
    const auto preset_digest = requireString(params, "base_preset_digest", "fork_render_pipeline");
    const auto destination = requireString(params, "destination_reference", "fork_render_pipeline");
    if (!isLowerHexDigest(preset_digest))
        throw std::invalid_argument("fork_render_pipeline base_preset_digest must be 64 lowercase hex characters");
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference || !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "fork_render_pipeline requires the project document resolver");
    if (!destination.starts_with("project://") || destination.find('#') != std::string::npos)
        return rejection("invalid_preset_destination", "Choose a new project:// file for the preset");
    try {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (!current.pipeline_preset || !current.pipeline_preset_bytes)
            return rejection("pipeline_preset_required", "The current rendering config does not select a preset");
        if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != preset_digest)
            return rejection("external_modification", "The selected preset changed since it was read");
        const auto path = dependencies_.resolve_document_path(destination);
        if (std::filesystem::exists(path))
            return rejection("preset_destination_exists", "The destination already exists: " + destination);
        const std::array documents{RenderConfigCandidateDocument{
            .reference = destination,
            .normalized_reference = dependencies_.normalize_reference(destination),
            .path = path,
            .operation = RenderConfigDocumentOperation::create,
            .expected = {RenderConfigDocumentExistence::missing, {}},
            .bytes = *current.pipeline_preset_bytes,
        }};
        auto result = queueRootEdit(base_digest, "fork_render_pipeline",
            [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                const auto parsed = Json::parse(candidate.bytes());
                auto pipeline = renderingSectionOf(parsed).at("pipeline");
                pipeline["preset"] = destination;
                candidate = candidate.withSectionValue("pipeline", pipeline.dump(2));
                return std::nullopt;
            }, documents, preset_digest);
        if (result.at("status") == "accepted") result["preset_reference"] = destination;
        return result;
    } catch (const RenderConfigSectionUnlocated &error) {
        return rejection("render_config_section_unlocated", error.what());
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

OrderedJson RenderConfigEditorService::getComputeTaskAuthoring(const Json &params) const {
    requireOnly(params, {}, "get_compute_task_authoring");
    if (!dependencies_.resolve_authoring_context)
        throw std::logic_error("render authoring context resolver is unavailable");
    const auto context = dependencies_.resolve_authoring_context(currentDocumentSet());
    // MG-3b: the tasks a render.compute node made. The lowering names each
    // task after the expanded id of the node that made it (a `name` parameter
    // is refused there), so the root document's own expansion -- the one the
    // lowering above partitioned -- says which listed tasks are the graph's,
    // and where the node that made each one is authored. The composition
    // appends these after the preset's tasks and refuses a feature task that
    // reuses one of their names, so a match is the node's task.
    std::map<std::string, GraphNodeOrigin, std::less<>> graph_tasks;
    {
        const auto expanded = expandGraphDocument(Json::parse(document_.bytes()));
        for (const auto &node : expanded.nodes) {
            if (node.at("op") != "render.compute") continue;
            const auto id = node.at("id").get<std::string>();
            if (const auto origin = expanded.origins.find(id); origin != expanded.origins.end())
                graph_tasks.emplace(id, origin->second);
        }
    }
    auto tasks = OrderedJson::array();
    for (const auto &task : context.config.value("compute_tasks", Json::array())) {
        const auto name = task.at("name").get<std::string>();
        const auto provenance = std::find_if(context.pass_provenance.begin(), context.pass_provenance.end(),
            [&](const auto &entry) { return entry.name == name; });
        const bool from_feature = provenance != context.pass_provenance.end() &&
            provenance->source == RenderPipelineProvenanceSource::feature;
        const auto graph_node = from_feature ? graph_tasks.end() : graph_tasks.find(name);
        const bool from_graph = graph_node != graph_tasks.end();
        const bool from_preset = !from_feature && !from_graph && context.pipeline_preset.has_value();
        // A node's task is authored in the root document even when the root
        // names a preset: the node is there, not in the preset.
        const auto reference = from_feature ? provenance->provider_reference
            : from_preset ? context.pipeline_preset->reference : dependencies_.source_reference;
        const auto *origin = from_feature ? "feature" : from_graph ? "graph_node" : from_preset ? "preset" : "root";
        OrderedJson entry{{"name", name}, {"kind", task.contains("ray_tracing") ? "ray_tracing" : "compute"},
                          {"origin", origin}, {"source_reference", reference}, {"editable", false},
                          {"declaration", task}};
        if (from_feature) entry["provider_feature"] = provenance->provider_feature;
        if (from_graph) {
            // Where the Graph tab finds the node: `scope` is the token the
            // graph edit ops take ("root", or the definition whose body holds
            // the node), `node` the id authored in that scope, and `instance`
            // the instance path that placed it ("" in the root scope).
            const auto &source = graph_node->second;
            auto instance = source.instance_path;
            if (instance.ends_with('/')) instance.pop_back();
            entry["graph_node"] = OrderedJson{{"scope", source.definition.empty() ? "root" : source.definition},
                                              {"node", source.node},
                                              {"instance", std::move(instance)}};
            entry["read_only_reason"] =
                "This task is made by a render.compute node in the graph. Edit the node in the Graph tab.";
        } else if (reference.starts_with("engine://")) {
            entry["read_only_reason"] = "This task belongs to a shared engine document. Copy it into the project to edit it.";
        } else if (reference == dependencies_.source_reference) {
            // MG-2b: the rendering root is a pelican.graph root, and its
            // `rendering` section cannot declare tasks (the lowering refuses
            // `compute_tasks` there, and it ran in the resolver above). A
            // render.compute node's task is caught above (MG-3b), so the
            // production resolver credits nothing else to the root; a task a
            // resolver does credit to it was still derived, so it is listed
            // with origin "root", read-only, and never edited here.
            entry["read_only_reason"] = "The task is generated, not declared in this document";
        } else {
            try {
                const auto bytes = from_preset && context.pipeline_preset_bytes ? *context.pipeline_preset_bytes
                    : readFileBytes(dependencies_.resolve_document_path(reference));
                auto source = OrderedJson::parse(bytes);
                const auto &config = from_feature ? featureSourceFragment(source, provenance->provider_feature)
                    : source.at("config");
                const auto declarations = config.find("compute_tasks");
                if (declarations == config.end()) throw std::runtime_error("The task is generated, not declared in this document");
                const auto original = std::find_if(declarations->begin(), declarations->end(),
                    [&](const auto &candidate) { return candidate.at("name") == name; });
                if (original == declarations->end()) throw std::runtime_error("The task is no longer present in its source document");
                entry["declaration"] = *original;
                entry["source_digest"] = digestJson(renderConfigSourceDigest(bytes));
                entry["editable"] = true;
            } catch (const std::exception &error) {
                entry["read_only_reason"] = error.what();
            }
        }
        // Qt's JSON transport cannot represent every uint64 task value as a number.
        entry["declaration_json"] = entry.at("declaration").dump(2);
        tasks.push_back(std::move(entry));
    }
    auto resources = OrderedJson::array();
    for (const auto &[field, kind] : std::array{
             std::pair{"render_targets", "image"}, std::pair{"buffers", "buffer"}}) {
        for (const auto &resource : context.config.value(field, Json::array())) {
            const auto name = resource.is_string() ? resource.get<std::string>()
                                                   : resource.at("name").get<std::string>();
            resources.push_back({{"name", name}, {"kind", kind},
                                 {"declaration_json", resource.dump(2)}});
        }
    }
    return {{"source_digest", digestJson(document_.sourceDigest())}, {"tasks", std::move(tasks)},
            {"resources", std::move(resources)}};
}

OrderedJson RenderConfigEditorService::getFrameNodeOrder(const Json &params) const {
    requireOnly(params, {}, "get_frame_node_order");
    return {{"source_digest", digestJson(document_.sourceDigest())},
            {"published_generation", dependencies_.runtime_snapshot().published_generation},
            {"orders", renderingSectionOrEmpty(Json::parse(document_.bytes()))
                           .value("frame_node_order", Json::object())}};
}

OrderedJson RenderConfigEditorService::getGraphInstances(const Json &params) const {
    requireOnly(params, {}, "get_graph_instances");
    const auto root = Json::parse(document_.bytes());
    // MG-2b: the document is always a pelican.graph root (anything else is
    // refused as render_config.legacy_root@1 before it becomes document_).
    auto instances = Json::array();
    auto nodes = root.at("nodes");
    for (const auto &node : nodes)
        if (node.at("op") == "graph.instance") instances.push_back(node);
    return {{"source_reference", dependencies_.source_reference},
        // N-8: what the root document is. Since MG-2b it is always
        // "pelican.graph"; the field stays because Studio reads it.
        {"root_schema", root.value("schema", std::string{})},
        {"source_digest", digestJson(document_.sourceDigest())},
        {"instances", std::move(instances)},
        // The whole root array too: an editor that connects an input has to
        // name a producer that is already there, and has to avoid an id that
        // is already taken.
        {"nodes", std::move(nodes)},
        {"definitions", root.value("definitions", Json::object())},
        // N-6: the editor's own placement for the root scope. A definition's
        // travels inside its body, so this is the one that needs naming, and
        // reporting it here is what keeps the canvas from needing a read of
        // its own for something the document already carries.
        {"layout", root.value("layout", Json::object())}};
}

OrderedJson RenderConfigEditorService::editGraphInstances(const Json &params) {
    constexpr auto method = "edit_graph_instances";
    requireOnly(params, {"base_source_digest", "attach", "detach"}, method);
    const auto digest = requireString(params, "base_source_digest", method);
    const auto attached = params.value("attach", Json::array());
    const auto detached = params.value("detach", Json::array());
    return queueRootEdit(digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            try {
                const auto edited = Pelican::editGraphInstances(Json::parse(candidate.bytes()), attached, detached);
                (void)partitionGraphDocument(edited);
                candidate = candidate.withRootValue("nodes", edited.at("nodes").dump(2));
                return std::nullopt;
            } catch (const std::exception &error) {
                return rejection("invalid_graph_attachment", error.what());
            }
        });
}

OrderedJson RenderConfigEditorService::editGraphDocument(const Json &params) {
    constexpr auto method = "edit_graph_document";
    requireOnly(params, {"base_source_digest", "ops"}, method);
    const auto digest = requireString(params, "base_source_digest", method);
    if (!params.contains("ops") || !params.at("ops").is_array())
        return rejection("invalid_graph_document", "Provide an array of graph document ops");
    const auto &ops = params.at("ops");
    // N-6: an op list that is nothing but placement changes nothing the engine
    // evaluates. It still takes a ticket and the same CAS -- two editors must
    // not both win -- but it is committed to the source without compiling or
    // republishing a candidate, because there is no candidate: the composed
    // graph, the catalog and the frame plan are the same ones as before.
    const bool layout_only =
        !ops.empty() &&
        std::all_of(ops.begin(), ops.end(), [](const Json &operation) {
            return operation.is_object() &&
                   operation.value("op", std::string{}) == "set_layout";
        });
    // One ticket, one CAS, one candidate: the same queue edit_graph_instances
    // uses, so two edits in the same frame cannot both win.
    return queueRootEdit(digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            try {
                const auto source = Json::parse(candidate.bytes());
                const auto edited = Pelican::editGraphDocument(source, ops);
                (void)partitionGraphDocument(edited);
                candidate = candidate.withRootValue("nodes", edited.at("nodes").dump(2));
                // An op may now address a definition body. The root `nodes`
                // write-back alone would drop that edit on the floor, so the
                // definitions travel too -- and only when they moved, so a
                // root-only edit still leaves those bytes exactly as authored.
                if (edited.contains("definitions") &&
                    edited.at("definitions") != source.value("definitions", Json{}))
                    candidate = candidate.withRootValue(
                        "definitions", edited.at("definitions").dump(2));
                // And the root scope's own placement, on the same terms.
                if (edited.value("layout", Json::object()) !=
                    source.value("layout", Json::object()))
                    candidate = candidate.withRootValue(
                        "layout", edited.value("layout", Json::object()).dump(2));
                return std::nullopt;
            } catch (const GraphRegistryError &error) {
                return rejection(error.code(), error.what(),
                                 OrderedJson::parse(error.detail().dump()));
            } catch (const std::exception &error) {
                return rejection("invalid_graph_document", error.what());
            }
        }, {}, std::nullopt, layout_only);
}

OrderedJson RenderConfigEditorService::getGraphValues(const Json &params) const {
    constexpr auto method = "get_graph_values";
    requireOnly(params, {"instance"}, method);
    const auto instance = requireString(params, "instance", method);
    // Read the publishing pass's snapshot. Evaluating the graph here would
    // advance its clocks and consume its events, so an unpublished graph
    // reports no values rather than freshly invented ones.
    const auto snapshot = dependencies_.graph_values ? dependencies_.graph_values()
                                                     : ValueGraph::PublishedInstanceInputs{};
    const auto prefix = instance + "/";
    auto inputs = OrderedJson::array();
    for (const auto &[key, value] : snapshot.inputs) {
        if (!key.starts_with(prefix)) continue;
        const auto port = key.substr(prefix.size());
        if (port.empty() || port.find('/') != std::string::npos) continue; // A nested instance's port.
        OrderedJson entry{{"port", port}, {"value", value.value},
            {"source", std::string{ValueGraph::instanceInputSourceName(value.source)}}};
        if (value.source == ValueGraph::InstanceInputSource::connected)
            entry["connected"] = OrderedJson{{"node", value.node}, {"port", value.port}};
        else
            entry["parameter"] = value.parameter;
        inputs.push_back(std::move(entry));
    }
    return {{"source_digest", digestJson(document_.sourceDigest())},
            {"instance", instance},
            {"published", snapshot.published},
            {"generation", snapshot.generation},
            {"frame", snapshot.frame},
            {"inputs", std::move(inputs)}};
}

OrderedJson RenderConfigEditorService::getGraphNodeValues(const Json &params) const {
    constexpr auto method = "get_graph_node_values";
    requireOnly(params, {"instance"}, method);
    // An omitted `instance` and an empty one are the same question: the root
    // scope. Anything else must still be a string, so a caller that passes a
    // number learns that instead of being quietly answered for the root.
    std::string instance;
    if (params.find("instance") != params.end())
        instance = requireString(params, "instance", method);
    // The same snapshot get_graph_values reads, and for the same reason:
    // evaluating the graph here would advance its clocks and consume its
    // events, so an unpublished graph reports no nodes rather than freshly
    // invented ones.
    const auto snapshot = dependencies_.graph_values ? dependencies_.graph_values()
                                                     : ValueGraph::PublishedInstanceInputs{};
    const auto reply = [&](OrderedJson nodes) {
        return OrderedJson{{"instance", instance},
                           {"generation", snapshot.generation},
                           {"frame", snapshot.frame},
                           {"published", snapshot.published},
                           // These are the values the rendering evaluation of
                           // the document used. The animation phase evaluates
                           // the same document against its own state, so in a
                           // document driven by events or triggers the two can
                           // differ for good; naming the side here is what
                           // keeps this from reading as "what animation ate".
                           {"source", "rendering"},
                           {"nodes", std::move(nodes)}};
    };
    if (!snapshot.published || !snapshot.graph) return reply(OrderedJson::array());
    // A pelican.graph root (MG-2b), so it always has a graph to expand.
    const auto root = Json::parse(document_.bytes());
    // Node ids come from the authored document's expansion, so every node the
    // canvas draws is listed -- including the rendering and animation domain
    // nodes the values partition has no value for, which are listed with no
    // outputs rather than left out. The lowering's own synthesised nodes
    // ($upload/..., $binding/...) are not in the expansion and so cannot leak
    // into a scope as a second source of truth.
    ExpandedGraphDocument expanded;
    try {
        expanded = expandGraphDocument(root);
    } catch (const std::exception &error) {
        return rejection("invalid_graph_document", error.what());
    }
    // The published generation's node values, resolved by name once here
    // rather than once per frame in the draw path.
    const auto ids = snapshot.graph->nodeIds();
    std::map<std::string, double, std::less<>> values;
    for (std::size_t index = 0; index < ids.size() && index < snapshot.node_values.size(); ++index)
        if (snapshot.node_values[index]) values.emplace(ids[index], *snapshot.node_values[index]);
    const auto &output_nodes = snapshot.graph->outputNodes();
    const auto outputValue = [&](const std::string &name) -> std::optional<double> {
        const auto found = output_nodes.find(name);
        if (found == output_nodes.end() || found->second.empty()) return std::nullopt;
        const auto index = found->second.front();
        if (index >= snapshot.node_values.size()) return std::nullopt;
        return snapshot.node_values[index];
    };
    const auto prefix = instance.empty() ? std::string{} : instance + "/";
    auto nodes = OrderedJson::array();
    for (const auto &node : expanded.nodes) {
        const auto id = node.at("id").get<std::string>();
        const auto origin = expanded.origins.find(id);
        if (origin == expanded.origins.end()) continue;
        // The expansion tree, not the id's spelling: a node inside a nested
        // instance belongs to the scope that contains it, and stays listed
        // under the path that reaches it.
        if (!origin->second.instance_path.starts_with(prefix)) continue;
        auto outputs = OrderedJson::object();
        // A values-domain node: the value the node itself produced, under each
        // port it declares. Every such node declares one scalar port.
        const auto declared = node.value("outputs", Json::array());
        if (const auto value = values.find(id); value != values.end())
            for (const auto &port : declared)
                if (port.is_string()) outputs[port.get<std::string>()] = value->second;
        // An animation-domain node produces no value of its own, but the
        // scalars it consumed this frame are published by the values partition
        // as `$pose/<node>/<port>`. Those are what an observer of a pose node
        // can actually see, so they are listed under the port that took them.
        // The object is bound first: items() borrows it, and a temporary would
        // be gone before the loop reads a single port.
        const auto inputs = node.value("inputs", Json::object());
        for (const auto &[port, endpoint] : inputs.items()) {
            (void)endpoint;
            if (outputs.contains(port)) continue;
            if (const auto value = outputValue(graphAnimationScalarOutput(id, port)))
                outputs[port] = *value;
        }
        nodes.push_back(OrderedJson{{"node", id.substr(prefix.size())},
                                    {"expanded", id},
                                    {"outputs", std::move(outputs)}});
    }
    return reply(std::move(nodes));
}

OrderedJson RenderConfigEditorService::getGraphCatalog(const Json &params) const {
    requireOnly(params, {}, "get_graph_catalog");
    return builtinGraphModuleRegistry().catalog();
}

OrderedJson RenderConfigEditorService::editFrameNodeOrder(const Json &params) {
    constexpr auto method = "edit_frame_node_order";
    requireOnly(params, {"base_source_digest", "orders"}, method);
    const auto digest = requireString(params, "base_source_digest", method);
    if (!params.contains("orders") || !params.at("orders").is_object())
        return rejection("invalid_frame_node_order", "Provide an object mapping graph names to node arrays");
    return queueRootEdit(digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            candidate = candidate.withSectionValue("frame_node_order", params.at("orders").dump(2));
            return std::nullopt;
        });
}

OrderedJson RenderConfigEditorService::setMaterialRouteDefault(const Json &params) {
    constexpr auto method = "set_material_route_default";
    requireOnly(params, {"base_source_digest", "material_route_default"}, method);
    const auto digest = requireString(params, "base_source_digest", method);
    if (!params.contains("material_route_default")) {
        throw std::invalid_argument(
            std::string{method} + " requires material_route_default");
    }
    const auto value = requestedMaterialRouteDefault(
        params.at("material_route_default"), method);
    if (value == MaterialRouteDefault::forward &&
        !runtime_shader_compiler_available) {
        return rejection(
            std::string{materialRouteDefaultUnavailableCode},
            "material_route_default \"forward\" needs the runtime shader "
            "compiler to build the forward version of the engine's built-in "
            "materials; this build has none",
            {{"value", "forward"},
             {"reason", "runtime_shader_compiler_disabled"}});
    }
    auto accepted = queueRootEdit(digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            candidate = candidate.withSectionValue(
                materialRouteDefaultKey,
                Json(std::string{materialRouteDefaultName(value)}).dump());
            return std::nullopt;
        });
    // The engine reads the key once at start; a committed change says so.
    if (accepted.value("status", std::string{}) == "accepted" &&
        !pending_.empty()) {
        pending_.back().restart_required = true;
    }
    return accepted;
}

OrderedJson RenderConfigEditorService::editComputeTask(const Json &params) {
    requireOnly(params, {"base_source_digest", "base_task_source_digest", "task_name", "task"}, "edit_compute_task");
    return changeComputeTask(params, false);
}

OrderedJson RenderConfigEditorService::removeComputeTask(const Json &params) {
    requireOnly(params, {"base_source_digest", "base_task_source_digest", "task_name"}, "remove_compute_task");
    return changeComputeTask(params, true);
}

OrderedJson RenderConfigEditorService::addComputeTask(const Json &params) {
    constexpr auto method = "add_compute_task";
    requireOnly(params, {"base_source_digest", "destination_reference", "task"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto destination = requireString(params, "destination_reference", method);
    if (!params.contains("task") || !params.at("task").is_object())
        return rejection("invalid_compute_task", "Provide a task declaration object");
    const auto name = requireString(params.at("task"), "name", method);
    if (name.empty()) return rejection("invalid_compute_task", "The task name must not be empty");
    if (!destination.starts_with("project://") || destination.find('#') != std::string::npos)
        return rejection("invalid_task_destination", "Choose a new project:// file for the task feature");
    try {
        const auto catalog = getComputeTaskAuthoring(Json::object());
        for (const auto &task : catalog.at("tasks"))
            if (task.at("name") == name)
                return rejection("compute_task_exists", "A task already uses this name: " + name);
        const auto path = dependencies_.resolve_document_path(destination);
        if (std::filesystem::exists(path))
            return rejection("task_destination_exists", "The destination already exists: " + destination);
        const OrderedJson feature{{"schema", "pelican.render_feature"}, {"version", 1},
            {"name", name}, {"compute_tasks", OrderedJson::array({params.at("task")})}};
        const std::array documents{RenderConfigCandidateDocument{
            .reference = destination, .normalized_reference = dependencies_.normalize_reference(destination),
            .path = path, .operation = RenderConfigDocumentOperation::create,
            .expected = {RenderConfigDocumentExistence::missing, {}}, .bytes = feature.dump(2)}};
        return queueRootEdit(base_digest, method,
            [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                if (!candidate.hasFeaturesArray())
                    candidate = AuthoredRenderConfigDocument::initialize(candidate.bytes());
                candidate = candidate.withFeatureAdded(destination);
                return std::nullopt;
            }, documents);
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

OrderedJson RenderConfigEditorService::changeComputeTask(const Json &params, bool remove) {
    const auto method = remove ? "remove_compute_task" : "edit_compute_task";
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto task_digest = requireString(params, "base_task_source_digest", method);
    const auto name = requireString(params, "task_name", method);
    if (!isLowerHexDigest(base_digest) || !isLowerHexDigest(task_digest))
        throw std::invalid_argument(std::string{method} + " requires 64 lowercase hex source digests");
    if (!remove && (!params.contains("task") || !params.at("task").is_object() || params.at("task").value("name", std::string{}) != name))
        return rejection("invalid_compute_task", "The task declaration must retain its name");
    try {
        const auto catalog = getComputeTaskAuthoring(Json::object());
        const auto &tasks = catalog.at("tasks");
        const auto selected = std::find_if(tasks.begin(), tasks.end(), [&](const auto &entry) { return entry.at("name") == name; });
        if (selected == tasks.end() || !selected->at("editable").get<bool>())
            return rejection("compute_task_not_editable", "Select a task declared in a project-owned document");
        if (selected->at("source_digest").at("hex") != task_digest)
            return rejection("external_modification", "The task source changed since it was read");
        // An editable task is declared in a project feature or a project
        // preset, never in the rendering root (MG-2b: the catalog lists the
        // root's tasks read-only, so the check above already refused them).
        const auto reference = selected->at("source_reference").get<std::string>();
        const auto path = dependencies_.resolve_document_path(reference);
        const auto bytes = readFileBytes(path);
        if (renderConfigSourceDigest(bytes) != task_digest)
            return rejection("external_modification", "The task source changed while the edit was prepared");
        auto source = OrderedJson::parse(bytes);
        auto &config = source.value("schema", std::string{}) == "pelican.render_feature"
            ? featureSourceFragment(source, selected->at("provider_feature").get<std::string>())
            : source.at("config");
        auto &declarations = config.at("compute_tasks");
        const auto original = std::find_if(declarations.begin(), declarations.end(), [&](const auto &entry) { return entry.at("name") == name; });
        if (original == declarations.end()) return rejection("compute_task_not_editable", "The source task is absent");
        if (remove) declarations.erase(original);
        else *original = params.at("task");
        const std::array documents{RenderConfigCandidateDocument{
            .reference = reference, .normalized_reference = dependencies_.normalize_reference(reference),
            .path = path, .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present, task_digest}, .bytes = source.dump(2)}};
        return queueRootEdit(base_digest, method,
            [](AuthoredRenderConfigDocument &) -> std::optional<OrderedJson> { return std::nullopt; }, documents);
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

OrderedJson RenderConfigEditorService::editRegionReplacement(const Json &params) {
    constexpr auto method = "edit_region_replacement";
    requireOnly(params, {"base_source_digest", "base_preset_digest", "graph", "region", "provider"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto preset_digest = requireString(params, "base_preset_digest", method);
    const auto graph_name = requireString(params, "graph", method);
    const auto region = requireString(params, "region", method);
    if (!isLowerHexDigest(base_digest) || !isLowerHexDigest(preset_digest))
        throw std::invalid_argument("edit_region_replacement requires 64 lowercase hex source digests");
    if (!params.contains("provider") ||
        (!params.at("provider").is_null() &&
         (!params.at("provider").is_string() || params.at("provider").get_ref<const std::string &>().empty())))
        throw std::invalid_argument("provider must be a non-empty name or null to use the authored nodes");
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference || !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "edit_region_replacement requires the project document resolver");
    try {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (!current.pipeline_preset || !current.pipeline_preset_bytes ||
            !current.pipeline_preset->reference.starts_with("project://"))
            return rejection("project_preset_required", "Fork the selected preset into the project before editing region replacements");
        if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != preset_digest)
            return rejection("external_modification", "The selected preset changed since it was read");
        auto preset = OrderedJson::parse(*current.pipeline_preset_bytes);
        auto &graphs = preset.at("config").at("rendering_passes");
        auto graph = std::find_if(graphs.begin(), graphs.end(), [&](const auto &value) {
            return value.at("name") == graph_name;
        });
        if (graph == graphs.end())
            return rejection("render_graph_not_found", "The selected graph is not declared in the project preset");
        const auto &passes = graph->at("passes");
        const bool declared = std::any_of(passes.begin(), passes.end(), [&](const auto &pass) {
            const auto tags = pass.find("regions");
            return tags != pass.end() && std::find(tags->begin(), tags->end(), region) != tags->end();
        });
        if (!declared)
            return rejection("region_not_found", "The selected region is not declared in the project preset");
        auto requests = graph->value("region_replacements", OrderedJson::array());
        const auto request = std::find_if(requests.begin(), requests.end(), [&](const auto &value) {
            return value.at("region") == region;
        });
        // Removing the request restores the authored graph, including edits made
        // while a replacement was selected. Never materialize provider output here.
        if (params.at("provider").is_null()) {
            if (request != requests.end()) requests.erase(request);
        } else if (request != requests.end()) {
            (*request)["provider"] = params.at("provider");
        } else {
            requests.push_back({{"region", region}, {"provider", params.at("provider")}});
        }
        if (requests.empty()) graph->erase("region_replacements");
        else (*graph)["region_replacements"] = std::move(requests);
        const auto &reference = current.pipeline_preset->reference;
        const std::array documents{RenderConfigCandidateDocument{
            .reference = reference,
            .normalized_reference = dependencies_.normalize_reference(reference),
            .path = dependencies_.resolve_document_path(reference),
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present, preset_digest},
            .bytes = preset.dump(2),
        }};
        return queueRootEdit(base_digest, method,
            [](AuthoredRenderConfigDocument &) -> std::optional<OrderedJson> { return std::nullopt; },
            documents, preset_digest);
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

namespace {

// The replacement rule edit_preset_pass applies to one editable declaration,
// whichever document holds it (a project preset, a project feature or, F-6, a
// direct root): each pass type has its own field set, an omitted field keeps
// its value, and a raster pass's `raster_state: null` removes the member.
// `routing_config` is the object that owns `material_routing`; only a preset
// has one here.
void applyEditablePassFields(const Json &params, OrderedJson &pass, OrderedJson *routing_config,
                             const char *method) {
    if (isEditableAuxiliaryPass(pass.at("type").get<std::string>())) {
        requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "output", "shader", "ray_query"}, method);
        bool changed = false;
        for (const auto *key : {"output", "shader", "ray_query"}) {
            if (!params.contains(key)) continue;
            changed = true;
            if (std::string_view{key} == "shader") {
                const auto type = pass.at("type").get<std::string>();
                if (type == "shadow_depth") requireOnly(params.at(key), {"vertex"}, "shadow_depth shader");
                else if (type == "velocity" || type == "picking")
                    requireOnly(params.at(key), {"vertex", "fragment", "skinned_vertex"}, "geometry shader");
                else requireOnly(params.at(key), {"vertex", "fragment"}, "auxiliary shader");
                if (!pass.contains(key)) {
                    pass[key] = {{"vertex", "engine://" + type}};
                    if (type != "shadow_depth") pass[key]["fragment"] = "engine://" + type;
                }
                for (const auto &[field, value] : params.at(key).items()) {
                    if (field == "skinned_vertex" && value.is_null()) pass[key].erase(field);
                    else pass[key][field] = value;
                }
            } else pass[key] = params.at(key);
        }
        if (!changed) throw std::invalid_argument("Auxiliary pass editing requires at least one editable field");
    } else if (pass.at("type") == "raster") {
        requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "draw", "raster_state", "ray_query"}, method);
        bool changed = false;
        for (const auto *key : {"input", "output", "shader", "draw", "raster_state", "ray_query"}) {
            if (!params.contains(key)) continue;
            changed = true;
            if (std::string_view{key} == "raster_state" && params.at(key).is_null()) pass.erase(key);
            else pass[key] = params.at(key);
        }
        if (!changed) throw std::invalid_argument("Raster pass editing requires at least one editable field");
        const auto &output = pass.at("output");
        const auto &color = output.at("color");
        (void)parseRasterPassContract(Json(pass), color.is_array() ? color.size() : 1,
            output.contains("depth") && !output.at("depth").is_null(), "edit_preset_pass");
    } else if (pass.at("type") == "material") {
        requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "ray_query", "material_settings", "material_routing"}, method);
        if (!params.contains("input") && !params.contains("output") && !params.contains("ray_query") &&
            !params.contains("material_settings") && !params.contains("material_routing"))
            throw std::invalid_argument("Material pass editing requires at least one editable field");
        for (const auto *key : {"input", "output"})
            if (params.contains(key)) pass[key] = params.at(key);
        if (params.contains("material_settings")) {
            const auto &settings = params.at("material_settings");
            requireOnly(settings, {"material_range", "material_filter", "material_variant", "material_contract",
                "gpu_draw_source", "material_outputs", "material_output_states", "screen_inputs",
                "surface_resources", "material_resources"}, "material_settings");
            for (const auto &[key, value] : settings.items()) {
                if (value.is_null()) pass.erase(key);
                else pass[key] = value;
            }
        }
        if (params.contains("material_routing")) {
            if (!routing_config) throw std::invalid_argument("Material routing belongs to the rendering preset");
            if (params.at("material_routing").is_null()) routing_config->erase("material_routing");
            else (*routing_config)["material_routing"] = params.at("material_routing");
        }
    } else {
        requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "ray_query", "uses_light_data", "push_constants", "clear_colors"}, method);
        for (const auto *key : {"input", "output", "shader"}) {
            if (!params.contains(key)) throw std::invalid_argument(std::string{method} + " requires " + key);
            pass[key] = params.at(key);
        }
        // G-3: what the pass asks of the engine and what it clears its
        // outputs to. Omitted keeps the value; null removes the member.
        for (const auto *key : {"uses_light_data", "push_constants", "clear_colors"}) {
            if (!params.contains(key)) continue;
            if (params.at(key).is_null()) pass.erase(key);
            else pass[key] = params.at(key);
        }
    }
    if (params.contains("ray_query")) pass["ray_query"] = params.at("ray_query");
}

} // namespace

OrderedJson RenderConfigEditorService::editPresetPass(const Json &params) {
    constexpr auto method = "edit_preset_pass";
    requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "draw", "raster_state", "ray_query", "material_settings", "material_routing", "uses_light_data", "push_constants", "clear_colors"}, method);
    (void)parseRasterRayQuery(params);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const bool feature_edit = params.contains("base_feature_digest");
    if (feature_edit && params.contains("base_preset_digest"))
        throw std::invalid_argument("edit_preset_pass requires only the selected source document digest");
    // F-6: with neither document digest the request names the root itself;
    // base_source_digest is then the digest of the document being edited.
    const bool root_edit = !feature_edit && !params.contains("base_preset_digest");
    const auto preset_digest = root_edit ? base_digest
        : requireString(params, feature_edit ? "base_feature_digest" : "base_preset_digest", method);
    const auto graph = requireString(params, "graph", method);
    const auto name = requireString(params, "pass_name", method);
    if (!isLowerHexDigest(base_digest) || !isLowerHexDigest(preset_digest))
        throw std::invalid_argument("edit_preset_pass requires 64 lowercase hex source digests");
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference || !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "edit_preset_pass requires the project document resolver");
    try {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (root_edit && !current.pipeline_preset)
            return editRootPass(params, base_digest, graph, name, current);
        // A preset root's passes live in the preset, which is edited by its
        // own digest once forked; the root is never the place to write them.
        if (root_edit)
            return rejection("project_preset_required", "Fork the selected preset into the project before editing its passes");
        if (!feature_edit && (!current.pipeline_preset || !current.pipeline_preset_bytes ||
            !current.pipeline_preset->reference.starts_with("project://")))
            return rejection("project_preset_required", "Fork the selected preset into the project before editing its passes");
        std::string reference;
        std::string bytes;
        std::string authored_name = name;
        std::string provider;
        if (feature_edit) {
            if (params.contains("material_routing"))
                return rejection("feature_routing_not_editable", "Material routing belongs to the rendering preset, not a feature pass");
            bool graph_contains_pass = false;
            for (const auto &candidate_graph : current.config.at("rendering_passes"))
                if (candidate_graph.at("name") == graph)
                    for (const auto &node : candidate_graph.at("passes"))
                        if (node.at("name") == name) graph_contains_pass = true;
            const auto source = std::find_if(current.pass_provenance.begin(), current.pass_provenance.end(),
                [&](const auto &entry) { return entry.name == name && entry.source == RenderPipelineProvenanceSource::feature; });
            if (!graph_contains_pass || source == current.pass_provenance.end() || source->provider_reference.empty() ||
                source->provider_reference.starts_with("engine://"))
                return rejection("feature_pass_not_editable", "The selected node is not a project feature pass in this graph");
            reference = source->provider_reference;
            provider = source->provider_feature;
            if (!source->authored_name.empty() && source->authored_name != name)
                return rejection("feature_pass_not_editable", "Generated feature variants must be edited in their source document");
            if (!source->authored_name.empty()) authored_name = source->authored_name;
            bytes = readFileBytes(dependencies_.resolve_document_path(reference));
        } else {
            reference = current.pipeline_preset->reference;
            bytes = *current.pipeline_preset_bytes;
        }
        if (renderConfigSourceDigest(bytes) != preset_digest)
            return rejection("external_modification", "The selected pass source changed since it was read");
        auto preset = OrderedJson::parse(bytes);
        auto *pass = feature_edit ? findFeatureEditablePass(preset, authored_name, provider) : findPresetEditablePass(preset, graph, name);
        if (!pass) return rejection("preset_pass_not_editable", "The selected node is not an editable pass in the project preset");
        applyEditablePassFields(params, *pass, feature_edit ? nullptr : &preset.at("config"), method);
        const std::array documents{RenderConfigCandidateDocument{
            .reference = reference,
            .normalized_reference = dependencies_.normalize_reference(reference),
            .path = dependencies_.resolve_document_path(reference),
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present, preset_digest},
            .bytes = preset.dump(2),
        }};
        return queueRootEdit(base_digest, method,
            [](AuthoredRenderConfigDocument &) -> std::optional<OrderedJson> { return std::nullopt; },
            documents, feature_edit ? std::nullopt : std::optional{preset_digest});
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

OrderedJson RenderConfigEditorService::editRootPass(
    const Json &params, const std::string &base_digest, const std::string &graph,
    const std::string &pass_name, const ResolvedRenderConfigAuthoringContext &current) {
    constexpr auto method = "edit_preset_pass";
    if (params.contains("material_routing"))
        return rejection("root_routing_not_editable",
                         "Material routing belongs to a rendering preset; a direct root's routing is not edited through a pass",
                         {{"graph", graph}, {"pass_name", pass_name}});
    return queueRootEdit(base_digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            auto root = OrderedJson::parse(candidate.bytes());
            auto *pass = findRootEditablePass(root, graph, pass_name);
            if (!pass) {
                const auto source = std::find_if(current.pass_provenance.begin(), current.pass_provenance.end(),
                    [&](const auto &entry) { return entry.name == pass_name; });
                return rejection("root_pass_not_found",
                    "The direct root declares no editable pass " + pass_name + " in " + graph +
                        "; a feature's pass or one the engine adds is not the root's to edit",
                    {{"graph", graph}, {"pass_name", pass_name},
                     {"provenance", source == current.pass_provenance.end()
                                        ? OrderedJson(nullptr)
                                        : OrderedJson(provenanceSourceName(source->source))}});
            }
            applyEditablePassFields(params, *pass, nullptr, method);
            candidate = candidate.withSectionValue(
                "rendering_passes", renderingSectionOf(root).at("rendering_passes").dump(2));
            return std::nullopt;
        });
}

// The authored-pass tail shared by add_authored_pass and
// remove_authored_pass: candidate-set preflight -> ticket enqueue -> accepted
// response.  The operation name is the only difference between them.
OrderedJson RenderConfigEditorService::enqueueAuthoredPassTicket(
    std::string operation, const GateSnapshot &gate,
    const std::string &base_digest, AuthoredRenderConfigDocument root,
    RenderConfigCandidateDocumentSet candidate_documents,
    const std::string &reference, bool edits_additional_documents,
    const AuthoredCandidateCheck &check, bool restart_required) {
    try {
        // This is the decisive staged-file contrast: neither the feature nor
        // preset resolver may bypass the request-local document set.
        const auto candidate =
            dependencies_.resolve_authoring_context(candidate_documents);
        if (check) check(candidate);
    } catch (const std::exception &error) {
        return preflightRejection(error,
                                  {{"participant", "pelican.render_pipeline"},
                                   {"fragment_reference", reference}});
    }
    // G-6: the same candidate, by every variant the engine will compose it
    // with when it publishes.
    if (auto refused = preflightGraphVariants(
            candidate_documents,
            {{"participant", "pelican.render_pipeline"},
             {"fragment_reference", reference}})) {
        return *refused;
    }

    const auto ticket_id =
        "render-authored-pass-" + std::to_string(next_ticket_++);
    pending_.push_back(Ticket{
        .id = ticket_id,
        .accepted_gate_epoch = gate.epoch,
        .base_source_digest = base_digest,
        .candidate = std::move(root),
        .candidate_documents = std::move(candidate_documents),
        .operation = std::move(operation),
        .fragment_reference = reference,
        .edits_additional_documents = edits_additional_documents,
        .restart_required = restart_required,
    });
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return {{"ticket", ticket_id},
            {"status", "accepted"},
            {"base_source_digest", digestJson(base_digest)},
            {"fragment_reference", reference}};
}

OrderedJson RenderConfigEditorService::addAuthoredPass(
    const Json &params) {
    requireOnly(params,
                {"base_source_digest", "graph", "insert", "pass",
                 "material_route_default"},
                "add_authored_pass");
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path ||
        !dependencies_.resolve_authoring_context) {
        return rejection(
            "method_unavailable",
            "add_authored_pass requires the production document-set resolver",
            {{"method", "add_authored_pass"}});
    }
    const auto base_digest = requireString(
        params, "base_source_digest", "add_authored_pass");
    if (!isLowerHexDigest(base_digest)) {
        throw std::invalid_argument(
            "add_authored_pass base_source_digest must be 64 lowercase hex characters");
    }
    const auto graph = requireString(params, "graph", "add_authored_pass");
    const auto insert = requireString(params, "insert", "add_authored_pass");
    const auto pass = params.find("pass");
    if (pass == params.end() || !pass->is_object() ||
        !pass->contains("name") || !pass->at("name").is_string() ||
        pass->at("name").get_ref<const std::string &>().empty()) {
        throw std::invalid_argument(
            "add_authored_pass requires pass object with a non-empty string name");
    }
    // G-1: a material pass is authored the same way as a fullscreen one. Its
    // shape is the pass parser's to judge, against the composed candidate
    // (below); a raster pass draws no scene and is still not authored here.
    const auto pass_type = pass->value("type", std::string{});
    if (pass_type != "fullscreen" && pass_type != "material") {
        return rejection(
            "authored_pass_type_unavailable",
            "add_authored_pass authors fullscreen and material passes only",
            {{"requested_type", pass_type},
             {"supported_types", OrderedJson::array({"fullscreen", "material"})}});
    }
    // G-4: the pass and the project's material_route_default travel in one
    // candidate document set, so the candidate is judged with the routes the
    // new default gives every material.
    std::optional<MaterialRouteDefault> requested_route_default;
    if (const auto found = params.find("material_route_default");
        found != params.end()) {
        requested_route_default =
            requestedMaterialRouteDefault(*found, "add_authored_pass");
        if (*requested_route_default == MaterialRouteDefault::forward &&
            !runtime_shader_compiler_available) {
            return rejection(
                std::string{materialRouteDefaultUnavailableCode},
                "material_route_default \"forward\" needs the runtime shader "
                "compiler to build the forward version of the engine's "
                "built-in materials; this build has none",
                {{"value", "forward"},
                 {"reason", "runtime_shader_compiler_disabled"}});
        }
    }

    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(
            gate, base_digest, "add_authored_pass",
            "render pass authoring is currently disabled")) {
        return *rejected;
    }

    try {
        const auto current =
            dependencies_.resolve_authoring_context(currentDocumentSet());
        const auto graphs = current.config.find("rendering_passes");
        if (graphs == current.config.end() || !graphs->is_array()) {
            throw std::runtime_error(
                "resolved authoring context has no rendering_passes[]");
        }
        const auto selected = std::find_if(
            graphs->begin(), graphs->end(), [&](const auto &candidate) {
                return candidate.is_object() &&
                       candidate.value("name", std::string{}) == graph;
            });
        if (selected == graphs->end() || !selected->contains("passes") ||
            !selected->at("passes").is_array()) {
            return rejection(
                "render_graph_not_found",
                "the selected render graph is not present in the resolved authoring context",
                {{"graph", graph}});
        }
        bool valid_insert = false;
        const auto &passes = selected->at("passes");
        if (passes.empty()) {
            valid_insert = insert == "begin";
        } else {
            valid_insert =
                insert == "before:" +
                              passes.front().at("name").get<std::string>();
            for (const auto &candidate : passes) {
                valid_insert = valid_insert ||
                               insert == "after:" +
                                             candidate.at("name")
                                                 .get<std::string>();
            }
        }
        if (!valid_insert) {
            return rejection(
                "render_anchor_not_found",
                "choose an anchor returned by get_render_authoring_context",
                {{"graph", graph}, {"insert", insert}});
        }
    } catch (const std::exception &error) {
        return preflightRejection(error,
                                  {{"participant", "pelican.render_pipeline"}});
    }

    const auto id = authoredPassId(
        graph, pass->at("name").get_ref<const std::string &>());
    const auto reference = managedFragmentReference(id);
    const auto normalized =
        dependencies_.normalize_reference(reference);
    const auto path = dependencies_.resolve_document_path(reference);
    if (!isManagedFragmentPath(dependencies_.project_root, path)) {
        return rejection(
            "managed_fragment_namespace_violation",
            "managed render fragments must resolve below project://passes/authoring/",
            {{"reference", reference}});
    }
    std::error_code exists_error;
    if (managed_documents_.contains(normalized) ||
        std::filesystem::exists(path, exists_error)) {
        return rejection(
            "authored_pass_exists",
            "a managed fragment already owns this graph/pass identity",
            {{"reference", reference},
             {"graph", graph},
             {"pass", pass->at("name")}});
    }
    if (exists_error) {
        return rejection("external_modification", exists_error.message(),
                         {{"reference", reference}});
    }

    auto root = document_.hasFeaturesArray()
                    ? document_
                    : AuthoredRenderConfigDocument::initialize(
                          document_.bytes());
    root = root.withFeatureAdded(reference);
    bool restart_required = false;
    if (requested_route_default) {
        const auto current_route_default = readMaterialRouteDefault(
            renderingSectionOrEmpty(Json::parse(document_.bytes())));
        if (*requested_route_default != current_route_default) {
            try {
                root = root.withSectionValue(
                    std::string{materialRouteDefaultKey},
                    Json(std::string{materialRouteDefaultName(
                             *requested_route_default)})
                        .dump());
            } catch (const RenderConfigSectionUnlocated &error) {
                return rejection("render_config_section_unlocated",
                                 error.what(),
                                 {{"method", "add_authored_pass"}});
            }
            restart_required = true;
        }
    }
    const auto fragment_bytes = managedFragmentBytes(id, insert, *pass);

    std::vector<RenderConfigCandidateDocument> documents;
    documents.reserve(managed_documents_.size() + 2);
    // Addition exposes dependencies before the root reference. If root CAS
    // loses, the transaction rolls this create back.
    documents.push_back(RenderConfigCandidateDocument{
        .reference = reference,
        .normalized_reference = normalized,
        .path = path,
        .operation = RenderConfigDocumentOperation::create,
        .expected = {RenderConfigDocumentExistence::missing, {}},
        .bytes = fragment_bytes,
    });
    documents.push_back(RenderConfigCandidateDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .operation = RenderConfigDocumentOperation::replace,
        .expected = {RenderConfigDocumentExistence::present,
                     document_.sourceDigest()},
        .bytes = root.bytes(),
    });
    for (const auto &[key, managed] : managed_documents_) {
        (void)key;
        documents.push_back(RenderConfigCandidateDocument{
            .reference = managed.reference,
            .normalized_reference = managed.normalized_reference,
            .path = managed.path,
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present,
                         managed.digest},
            .bytes = managed.bytes,
        });
    }
    RenderConfigCandidateDocumentSet candidate_documents{
        source_normalized_reference_, std::move(documents)};
    AuthoredCandidateCheck check;
    if (pass_type == "material") {
        const auto features =
            dependencies_.runtime_snapshot
                ? dependencies_.runtime_snapshot().enabled_feature_names
                : std::vector<std::string>{};
        const bool hdr = std::ranges::find(features, "hdr") != features.end();
        check = [graph, name = pass->at("name").get<std::string>(), hdr](
                    const ResolvedRenderConfigAuthoringContext &candidate) {
            validateAuthoredMaterialPass(candidate.config, graph, name, hdr);
        };
    }
    return enqueueAuthoredPassTicket("add_authored_pass", gate, base_digest,
                                     std::move(root),
                                     std::move(candidate_documents),
                                     reference, false, check,
                                     restart_required);
}

OrderedJson RenderConfigEditorService::removeAuthoredPass(
    const Json &params) {
    requireOnly(params, {"base_source_digest", "fragment_reference"},
                "remove_authored_pass");
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path ||
        !dependencies_.resolve_authoring_context) {
        return rejection(
            "method_unavailable",
            "remove_authored_pass requires the production document-set resolver",
            {{"method", "remove_authored_pass"}});
    }
    const auto base_digest = requireString(
        params, "base_source_digest", "remove_authored_pass");
    const auto reference = requireString(
        params, "fragment_reference", "remove_authored_pass");
    if (!isLowerHexDigest(base_digest)) {
        throw std::invalid_argument(
            "remove_authored_pass base_source_digest must be 64 lowercase hex characters");
    }
    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(
            gate, base_digest, "remove_authored_pass",
            "render pass authoring is currently disabled")) {
        return *rejected;
    }

    AuthoredRenderConfigDocument root = document_;
    try {
        root = root.withFeatureRemoved(reference);
    } catch (const std::invalid_argument &error) {
        return rejection("render_feature_not_enabled", error.what(),
                         {{"fragment_reference", reference}});
    }
    const auto normalized =
        dependencies_.normalize_reference(reference);
    // F-1: the render target fragment is not an authored pass. Removing it
    // would drop every editor-authored target at once, around the per-target
    // reference check edit_render_targets makes.
    if (normalized ==
        dependencies_.normalize_reference(renderTargetFragmentReference())) {
        return rejection(
            "render_target_not_editable",
            "the render target fragment is edited with edit_render_targets",
            {{"fragment_reference", reference}});
    }
    const auto managed = managed_documents_.find(normalized);
    if (managed != managed_documents_.end()) {
        try {
            if (renderConfigSourceDigest(
                    readFileBytes(managed->second.path)) !=
                managed->second.digest) {
                return rejection(
                    "external_modification",
                    "managed render fragment changed outside the editor",
                    {{"fragment_reference", reference}});
            }
        } catch (const std::exception &error) {
            return rejection("external_modification", error.what(),
                             {{"fragment_reference", reference}});
        }
    }
    const auto remaining = allFeatureReferences(root.bytes());
    const auto remaining_count = static_cast<std::size_t>(std::count_if(
        remaining.begin(), remaining.end(), [&](const auto &candidate) {
            return dependencies_.normalize_reference(candidate) == normalized;
        }));

    std::vector<RenderConfigCandidateDocument> documents;
    documents.reserve(managed_documents_.size() + 1);
    // Removal severs the reference first. If fragment deletion fails, the
    // transaction restores the root from its backup.
    documents.push_back(RenderConfigCandidateDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .operation = RenderConfigDocumentOperation::replace,
        .expected = {RenderConfigDocumentExistence::present,
                     document_.sourceDigest()},
        .bytes = root.bytes(),
    });
    for (const auto &[key, baseline] : managed_documents_) {
        const bool purge = key == normalized && remaining_count == 0;
        documents.push_back(RenderConfigCandidateDocument{
            .reference = baseline.reference,
            .normalized_reference = baseline.normalized_reference,
            .path = baseline.path,
            .operation = purge ? RenderConfigDocumentOperation::erase
                               : RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present,
                         baseline.digest},
            .bytes = purge ? std::string{} : baseline.bytes,
        });
    }
    RenderConfigCandidateDocumentSet candidate_documents{
        source_normalized_reference_, std::move(documents)};
    return enqueueAuthoredPassTicket("remove_authored_pass", gate,
                                     base_digest, std::move(root),
                                     std::move(candidate_documents),
                                     reference);
}

OrderedJson RenderConfigEditorService::editRenderTargets(const Json &params) {
    constexpr std::string_view method = "edit_render_targets";
    requireOnly(params, {"base_source_digest", "ops"}, method);
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path ||
        !dependencies_.resolve_authoring_context) {
        return rejection(
            "method_unavailable",
            "edit_render_targets requires the production document-set resolver",
            {{"method", method}});
    }
    const auto base_digest =
        requireString(params, "base_source_digest", method);
    if (!isLowerHexDigest(base_digest)) {
        throw std::invalid_argument(
            "edit_render_targets base_source_digest must be 64 lowercase hex characters");
    }
    const auto ops = params.find("ops");
    if (ops == params.end() || !ops->is_array() || ops->empty()) {
        throw std::invalid_argument(
            "edit_render_targets requires a non-empty ops array");
    }
    // The request's own shape is checked before anything is read: a malformed
    // op is a caller error, not a rejection of the declaration it carries.
    const auto targetName = [](const Json &op) -> std::string {
        const auto target = op.find("target");
        if (target == op.end() || !target->is_object() ||
            !target->contains("name") || !target->at("name").is_string() ||
            target->at("name").get_ref<const std::string &>().empty()) {
            throw std::invalid_argument(
                "edit_render_targets add and replace require a target object "
                "with a non-empty string name");
        }
        return target->at("name").get<std::string>();
    };
    for (const auto &op : *ops) {
        if (!op.is_object() || !op.contains("op") || !op.at("op").is_string())
            throw std::invalid_argument(
                "edit_render_targets ops entries require a string op");
        const auto kind = op.at("op").get<std::string>();
        if (kind == "add") {
            requireOnly(op, {"op", "target"}, method);
            (void)targetName(op);
        } else if (kind == "replace") {
            requireOnly(op, {"op", "name", "target"}, method);
            (void)requireString(op, "name", method);
            (void)targetName(op);
        } else if (kind == "remove") {
            requireOnly(op, {"op", "name"}, method);
            (void)requireString(op, "name", method);
        } else {
            throw std::invalid_argument(
                "edit_render_targets op must be add, replace or remove: " + kind);
        }
    }

    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(
            gate, base_digest, method,
            "render target authoring is currently disabled")) {
        return *rejected;
    }

    const auto reference = renderTargetFragmentReference();
    const auto normalized = dependencies_.normalize_reference(reference);
    const auto path = dependencies_.resolve_document_path(reference);
    if (!isManagedFragmentPath(dependencies_.project_root, path)) {
        return rejection(
            "managed_fragment_namespace_violation",
            "managed render fragments must resolve below project://passes/authoring/",
            {{"reference", reference}});
    }
    const auto managed = managed_documents_.find(normalized);
    const bool fragment_exists = managed != managed_documents_.end();
    if (fragment_exists) {
        try {
            if (renderConfigSourceDigest(readFileBytes(managed->second.path)) !=
                managed->second.digest) {
                return rejection(
                    "external_modification",
                    "the render target fragment changed outside the editor",
                    {{"fragment_reference", reference}});
            }
        } catch (const std::exception &error) {
            return rejection("external_modification", error.what(),
                             {{"fragment_reference", reference}});
        }
    } else {
        std::error_code exists_error;
        if (std::filesystem::exists(path, exists_error) || exists_error) {
            return rejection(
                "external_modification",
                exists_error ? exists_error.message()
                             : reference +
                                   " exists but the render config does not "
                                   "reference it: remove the file or add it "
                                   "back to features[]",
                {{"fragment_reference", reference}});
        }
    }

    ResolvedRenderConfigAuthoringContext current;
    OrderedJson targets = OrderedJson::array();
    try {
        current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (fragment_exists)
            targets = fragmentRenderTargets(managed->second.bytes);
    } catch (const std::exception &error) {
        return preflightRejection(error,
                                  {{"participant", "pelican.render_pipeline"},
                                   {"fragment_reference", reference}});
    }
    auto origins = renderTargetOrigins(current, document_.bytes(), targets);
    std::set<std::string, std::less<>> declared;
    for (auto &name : renderTargetNames(current.config))
        declared.insert(std::move(name));
    std::vector<std::string> initial_names;
    for (const auto &target : targets)
        if (target.is_object() && target.contains("name") &&
            target.at("name").is_string())
            initial_names.push_back(target.at("name").get<std::string>());

    const auto reserved = [&](const std::string &name) {
        return rejection(
            "render_target_reserved",
            "Render target name is reserved by the engine: " + name,
            {{"name", name}});
    };
    const auto exists = [&](const std::string &name) {
        return rejection(
            "render_target_exists",
            "A render target named " + name + " is already declared",
            {{"name", name}, {"source", origins.source(name)}});
    };
    const auto notEditable = [&](const std::string &name) {
        if (!declared.contains(name)) {
            return rejection("render_target_not_found",
                             "No render target named " + name + " is declared",
                             {{"name", name}});
        }
        return rejection(
            "render_target_not_editable",
            name + " is declared outside the editor's render target fragment",
            {{"name", name}, {"source", origins.source(name)}});
    };
    const auto findTarget = [&targets](const std::string &name) {
        return std::find_if(targets.begin(), targets.end(),
                            [&](const auto &target) {
                                return target.is_object() &&
                                       target.value("name", std::string{}) ==
                                           name;
                            });
    };

    for (const auto &op : *ops) {
        const auto kind = op.at("op").get<std::string>();
        if (kind == "add") {
            const auto name = targetName(op);
            if (isReservedRenderTargetName(name)) return reserved(name);
            if (declared.contains(name)) return exists(name);
            targets.push_back(OrderedJson(op.at("target")));
            declared.insert(name);
            // Named by this request: it is the fragment's from here on.
            origins.authoring.insert(name);
            continue;
        }
        const auto name = op.at("name").get<std::string>();
        if (isReservedRenderTargetName(name)) return reserved(name);
        const auto found = findTarget(name);
        if (found == targets.end()) return notEditable(name);
        if (kind == "remove") {
            targets.erase(found);
            declared.erase(name);
            continue;
        }
        const auto renamed = targetName(op);
        if (isReservedRenderTargetName(renamed)) return reserved(renamed);
        if (renamed != name && declared.contains(renamed)) return exists(renamed);
        *found = OrderedJson(op.at("target"));
        declared.erase(name);
        declared.insert(renamed);
        origins.authoring.insert(renamed);
    }

    // A name the fragment declared and no longer does must have nobody left
    // reading or writing it -- remove and rename both end here.
    std::vector<std::string> dropped;
    for (const auto &name : initial_names)
        if (findTarget(name) == targets.end()) dropped.push_back(name);
    if (!dropped.empty()) {
        std::map<std::string, std::vector<std::string>, std::less<>> users;
        try {
            users = renderTargetUsers(current.config);
        } catch (const std::exception &error) {
            return preflightRejection(error,
                                      {{"participant", "pelican.render_pipeline"},
                                       {"fragment_reference", reference}});
        }
        for (const auto &name : dropped) {
            const auto found = users.find(name);
            if (found == users.end() || found->second.empty()) continue;
            std::string listed;
            for (const auto &user : found->second)
                listed += (listed.empty() ? "" : ", ") + user;
            return rejection(
                "render_target_referenced",
                name + " is still used by " + listed,
                {{"name", name}, {"users", found->second}});
        }
    }

    // The declaration's own shape is the parser's to judge, in its words.
    try {
        (void)parseRenderTargetDefinitionsFromJson(
            Json{{"render_targets", Json(targets)}});
    } catch (const std::exception &error) {
        return preflightRejection(error,
                                  {{"participant", "pelican.render_targets"},
                                   {"fragment_reference", reference}});
    }

    const auto fragment_bytes = renderTargetFragmentBytes(
        fragment_exists ? std::optional<std::string_view>{managed->second.bytes}
                        : std::nullopt,
        std::move(targets));
    auto root = document_;
    if (!fragment_exists) {
        if (!root.hasFeaturesArray())
            root = AuthoredRenderConfigDocument::initialize(document_.bytes());
        root = root.withFeatureAdded(reference);
    }

    std::vector<RenderConfigCandidateDocument> documents;
    documents.reserve(managed_documents_.size() + 2);
    if (!fragment_exists) {
        // As add_authored_pass: the dependency exists before the root names it.
        documents.push_back(RenderConfigCandidateDocument{
            .reference = reference,
            .normalized_reference = normalized,
            .path = path,
            .operation = RenderConfigDocumentOperation::create,
            .expected = {RenderConfigDocumentExistence::missing, {}},
            .bytes = fragment_bytes,
        });
    }
    documents.push_back(RenderConfigCandidateDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .operation = RenderConfigDocumentOperation::replace,
        .expected = {RenderConfigDocumentExistence::present,
                     document_.sourceDigest()},
        .bytes = root.bytes(),
    });
    for (const auto &[key, baseline] : managed_documents_) {
        documents.push_back(RenderConfigCandidateDocument{
            .reference = baseline.reference,
            .normalized_reference = baseline.normalized_reference,
            .path = baseline.path,
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present,
                         baseline.digest},
            .bytes = key == normalized ? fragment_bytes : baseline.bytes,
        });
    }
    RenderConfigCandidateDocumentSet candidate_documents{
        source_normalized_reference_, std::move(documents)};
    return enqueueAuthoredPassTicket(std::string{method}, gate, base_digest,
                                     std::move(root),
                                     std::move(candidate_documents), reference,
                                     /*edits_additional_documents=*/true);
}

OrderedJson RenderConfigEditorService::getResult(
    const Json &params) const {
    requireOnly(params, {"ticket"}, "get_edit_result");
    const auto ticket = requireString(params, "ticket", "get_edit_result");
    const auto found = results_.find(ticket);
    if (found == results_.end()) {
        throw std::invalid_argument(
            "get_edit_result ticket was not issued by this session");
    }
    return found->second;
}

bool RenderConfigEditorService::ownsTicket(
    std::string_view ticket) const noexcept {
    return results_.contains(std::string{ticket});
}

void RenderConfigEditorService::completeCommitTransaction(
    const std::shared_ptr<CommitTransaction> &transaction,
    RenderConfigRuntimeApplyResult applied) noexcept {
    transaction->completion_arrived = true;
    if (inflight_ticket_ == transaction->ticket.id) {
        inflight_ticket_.reset();
    }
    const auto finish = [this, &transaction](OrderedJson result) {
        if (!result.contains("ticket")) {
            result["ticket"] = transaction->ticket.id;
        }
        results_[transaction->ticket.id] = result;
        completed_.push_back(std::move(result));
    };
    const auto rollback_source = [&transaction]() noexcept {
        std::string error;
        if (transaction->source_receipt &&
            transaction->source_receipt->changed()) {
            try {
                rollbackCommittedRenderConfigDocuments(
                    std::move(*transaction->source_receipt));
            } catch (const std::exception &caught) {
                error = caught.what();
            } catch (...) {
                error =
                    "unknown render config rollback failure";
            }
        }
        return error;
    };
    try {
        if (!transaction->source_external_modification.empty()) {
            finish(rejection(
                "external_modification",
                transaction->source_external_modification,
                {{"ticket", transaction->ticket.id},
                 {"expected_source_digest",
                  transaction->ticket.base_source_digest}}));
            return;
        }
        if (applied.staged || !applied.committed) {
            const auto rollback_error = rollback_source();
            auto details = OrderedJson{
                {"ticket", transaction->ticket.id},
                {"participant", "pelican.render_pipeline"},
            };
            if (!rollback_error.empty()) {
                details["rollback_error"] = rollback_error;
            }
            // G-4: a named refusal the renderer carried as
            // "<code>: <JSON>" keeps its code, and its fields beside
            // ticket and participant.
            if (rollback_error.empty() && !applied.staged) {
                if (auto refusal =
                        splitCodedRefusalMessage(applied.error);
                    refusal && isCarriedRefusalCode(refusal->code)) {
                    for (const auto &[key, value] :
                         refusal->fields.items()) {
                        if (!details.contains(key)) details[key] = value;
                    }
                    finish(rejection(refusal->code,
                                     carriedRefusalMessage(*refusal),
                                     std::move(details)));
                    return;
                }
            }
            if (rollback_error.empty()) {
                details = withPreflightRefusalFields(std::move(details),
                                                     applied.exception);
            }
            finish(rejection(
                rollback_error.empty()
                    ? "render_pipeline_preflight_failed"
                    : "render_config_rollback_failed",
                applied.error.empty()
                    ? (applied.staged
                           ? "render pipeline completion returned staged twice"
                           : "render pipeline candidate was rejected")
                    : applied.error,
                std::move(details)));
            return;
        }
        if (!transaction->source_commit_called) {
            finish(rejection(
                "render_pipeline_commit_protocol_error",
                "pelican.render_pipeline reported a commit without invoking the source commit callback",
                {{"ticket", transaction->ticket.id},
                 {"participant", "pelican.render_pipeline"}}));
            return;
        }

        document_ = std::move(transaction->ticket.candidate);
        if (transaction->ticket.candidate_documents) {
            for (const auto &candidate :
                 transaction->ticket.candidate_documents->documents()) {
                if (candidate.normalized_reference ==
                    source_normalized_reference_) {
                    continue;
                }
                if (candidate.operation ==
                    RenderConfigDocumentOperation::erase) {
                    managed_documents_.erase(
                        candidate.normalized_reference);
                } else if (hasManagedFragmentMarker(candidate.bytes) &&
                           isManagedFragmentPath(
                               dependencies_.project_root,
                               candidate.path)) {
                    managed_documents_.insert_or_assign(
                        candidate.normalized_reference,
                        ManagedDocumentBaseline{
                            .reference = candidate.reference,
                            .normalized_reference =
                                candidate.normalized_reference,
                            .path = candidate.path,
                            .bytes = candidate.bytes,
                            .digest = renderConfigSourceDigest(
                                candidate.bytes),
                        });
                }
            }
        }
        auto completed = OrderedJson{
            {"ticket", transaction->ticket.id},
            {"status", "committed"},
            {"committed", true},
            {"published_generation", applied.published_generation},
            {"source_digest", digestJson(document_.sourceDigest())},
            {"source_commit_called",
             transaction->source_commit_called},
        };
        if (!transaction->ticket.operation.empty()) {
            completed["operation"] =
                transaction->ticket.operation;
        }
        if (!transaction->ticket.fragment_reference.empty()) {
            completed["fragment_reference"] =
                transaction->ticket.fragment_reference;
        }
        if (!applied.post_commit_error.empty()) {
            completed["post_commit_error"] =
                applied.post_commit_error;
        }
        if (transaction->ticket.restart_required) {
            // G-4: material_route_default is read once per engine start.
            completed["restart_required"] = true;
        }
        finish(std::move(completed));
    } catch (const std::exception &caught) {
        const auto rollback_error = rollback_source();
        OrderedJson details{{"ticket", transaction->ticket.id},
                            {"participant", "pelican.render_pipeline"},
                            {"rollback_error", rollback_error}};
        if (rollback_error.empty()) {
            details = withPreflightRefusalFields(std::move(details), caught);
        }
        finish(rejection(
            rollback_error.empty()
                ? "render_pipeline_preflight_failed"
                : "render_config_rollback_failed",
            caught.what(), std::move(details)));
    } catch (...) {
        (void)rollback_source();
        finish(rejection(
            "render_pipeline_preflight_failed",
            "unknown render pipeline completion failure",
            {{"ticket", transaction->ticket.id},
             {"participant", "pelican.render_pipeline"}}));
    }
}

void RenderConfigEditorService::commitPending() noexcept {
    if (inflight_ticket_) return;
    auto queue = std::move(pending_);
    pending_.clear();
    for (auto ticket_iterator = queue.begin();
         ticket_iterator != queue.end(); ++ticket_iterator) {
        auto &ticket = *ticket_iterator;
        auto finish = [&](OrderedJson result) {
            if (!result.contains("ticket")) result["ticket"] = ticket.id;
            results_[ticket.id] = result;
            completed_.push_back(std::move(result));
        };
        std::shared_ptr<CommitTransaction> transaction;
        try {
            const auto gate = gateSnapshot();
            if (!gate.can_edit || gate.epoch != ticket.accepted_gate_epoch) {
                finish(rejection(
                    "gate_closed",
                    "editor gate changed after render feature edit acceptance",
                    {{"ticket", ticket.id},
                     {"method", "edit_render_features"},
                     {"reason", primaryReason(gate.reasons)},
                     {"gate_epoch", gate.epoch}}));
                continue;
            }
            if (ticket.base_source_digest != document_.sourceDigest()) {
                finish(rejection(
                    "stale_source_digest",
                    "another render feature edit committed first",
                    {{"ticket", ticket.id},
                     {"current_source_digest", document_.sourceDigest()}}));
                continue;
            }
            const auto disk_digest = renderConfigSourceDigest(
                readFileBytes(dependencies_.source_path));
            if (disk_digest != ticket.base_source_digest) {
                finish(rejection(
                    "external_modification",
                    "render config source changed outside the editor",
                    {{"ticket", ticket.id},
                     {"expected_source_digest", ticket.base_source_digest},
                     {"actual_source_digest", disk_digest}}));
                continue;
            }

            // A preset edit can change another document while the root stays
            // byte-identical. It must still compile, check its CAS and publish.
            if (!ticket.edits_additional_documents && ticket.candidate.sourceDigest() ==
                document_.sourceDigest()) {
                const auto runtime =
                    dependencies_.runtime_snapshot();
                finish(OrderedJson{
                    {"ticket", ticket.id},
                    {"status", "committed"},
                    {"committed", true},
                    {"no_change", true},
                    {"published_generation",
                     runtime.published_generation},
                    {"source_digest",
                     digestJson(document_.sourceDigest())},
                    {"source_commit_called", false},
                });
                continue;
            }

            // N-6: an edit that only moved authored bytes the engine never
            // reads. There is no candidate to compile and nothing to publish,
            // so the source is written and the generation stays exactly where
            // it was -- which is what makes moving a node on the canvas free.
            if (ticket.source_only) {
                if (!ticket.candidate_documents)
                    throw std::logic_error(
                        "render config edit ticket has no candidate document set");
                auto receipt = commitRenderConfigCandidateDocuments(
                    dependencies_.project_root, *ticket.candidate_documents);
                (void)receipt;
                document_ = std::move(ticket.candidate);
                const auto runtime = dependencies_.runtime_snapshot();
                finish(OrderedJson{
                    {"ticket", ticket.id},
                    {"status", "committed"},
                    {"committed", true},
                    // Said out loud, because it is the whole point: the drawn
                    // graph was not recomposed for this edit.
                    {"recomposed", false},
                    {"published_generation", runtime.published_generation},
                    {"source_digest", digestJson(document_.sourceDigest())},
                    {"source_commit_called", true},
                });
                continue;
            }

            transaction = std::make_shared<CommitTransaction>(
                std::move(ticket));
            const auto project_root =
                dependencies_.project_root;
            std::function<void()> verify_preset;
            if (transaction->ticket.base_preset_digest) {
                verify_preset = [resolve = dependencies_.resolve_authoring_context,
                                 baseline = currentDocumentSet(),
                                 expected = *transaction->ticket.base_preset_digest] {
                    try {
                        const auto source = resolve(baseline);
                        if (!source.pipeline_preset_bytes || renderConfigSourceDigest(*source.pipeline_preset_bytes) != expected)
                            throw RenderConfigExternalModification("The source preset changed before the edit was saved");
                    } catch (const std::exception &error) {
                        throw RenderConfigExternalModification(error.what());
                    }
                };
            }
            const auto source_commit =
                [transaction, project_root, verify_preset = std::move(verify_preset)] {
                transaction->source_commit_called = true;
                try {
                    if (verify_preset) verify_preset();
                    if (!transaction->ticket.candidate_documents) {
                        throw std::logic_error(
                            "render config edit ticket has no candidate document set");
                    }
                    transaction->source_receipt =
                        commitRenderConfigCandidateDocuments(
                            project_root,
                            *transaction->ticket.candidate_documents);
                } catch (const RenderConfigExternalModification &error) {
                    // Production's reload participant converts callback
                    // exceptions to its result type. Preserve the CAS
                    // classification across that boundary.
                    transaction->source_external_modification =
                        error.what();
                    throw;
                }
            };
            const std::weak_ptr<const void> lifetime =
                lifetime_;
            const auto completion =
                [this, lifetime, transaction](
                    RenderConfigRuntimeApplyResult completed) noexcept {
                    if (lifetime.expired()) return;
                    completeCommitTransaction(
                        transaction, std::move(completed));
                };
            // Mark the transaction before calling into the runtime so a
            // synchronous completion cannot race with installing the
            // in-flight ticket after it has already cleared it.
            inflight_ticket_ = transaction->ticket.id;
            const auto applied = dependencies_.apply_candidate(
                *transaction->ticket.candidate_documents, source_commit,
                completion);
            if (applied.staged) {
                if (applied.staged_generation == 0) {
                    completeCommitTransaction(
                        transaction,
                        RenderConfigRuntimeApplyResult{
                            .error =
                                "render pipeline returned staged without a generation",
                        });
                } else if (!transaction->completion_arrived) {
                    const auto remaining =
                        std::next(ticket_iterator);
                    pending_.insert(
                        pending_.end(),
                        std::make_move_iterator(remaining),
                        std::make_move_iterator(queue.end()));
                    break;
                }
                continue;
            }
            if (!transaction->completion_arrived) {
                completeCommitTransaction(transaction, applied);
            }
        } catch (const RenderConfigExternalModification &error) {
            if (transaction && !transaction->completion_arrived) {
                if (transaction->source_external_modification.empty()) {
                    transaction->source_external_modification =
                        error.what();
                }
                completeCommitTransaction(
                    transaction,
                    RenderConfigRuntimeApplyResult{
                        .error = error.what(),
                    });
            } else if (!transaction) {
                finish(rejection(
                    "external_modification", error.what(),
                    {{"ticket", ticket.id},
                     {"expected_source_digest",
                      ticket.base_source_digest}}));
            }
        } catch (const std::exception &error) {
            if (transaction && !transaction->completion_arrived) {
                completeCommitTransaction(
                    transaction,
                    RenderConfigRuntimeApplyResult{
                        .error = error.what(),
                        .exception = std::current_exception(),
                    });
            } else if (!transaction) {
                finish(preflightRejection(
                    error, {{"ticket", ticket.id},
                            {"participant", "pelican.render_pipeline"}}));
            }
        } catch (...) {
            if (transaction && !transaction->completion_arrived) {
                completeCommitTransaction(
                    transaction,
                    RenderConfigRuntimeApplyResult{
                        .error =
                            "unknown render pipeline candidate failure",
                    });
            } else if (!transaction) {
                finish(rejection(
                    "render_pipeline_preflight_failed",
                    "unknown render pipeline candidate failure",
                    {{"ticket", ticket.id},
                     {"participant", "pelican.render_pipeline"}}));
            }
        }
    }
}

std::vector<OrderedJson>
RenderConfigEditorService::takeCompletedResults() {
    auto result = std::move(completed_);
    completed_.clear();
    return result;
}

} // namespace Pelican
