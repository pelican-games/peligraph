#include "../shader/raygrouplayout.hpp"
#include "renderconfigeditor.hpp"
#include "../../project/viewselect.hpp"
#include "../../project/assetsmanifest.hpp"
#include "../../project/packagemanifest.hpp"
#include "../../project/rendercomponent.hpp"
#include "../../project/rasterrayquery.hpp"
#include "../../project/rasterpass.hpp"
#include "../../project/featurecompose.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/graphvariantpolicy.hpp"
#include "../../project/graphpartition.hpp"
#include "../../project/graphtemplate.hpp"
#include "../../project/materialdrawloop.hpp"
#include "../../project/materialpassdecompose.hpp"
#include "../../project/renderconfigsection.hpp"
#include "renderconfigtransaction.hpp"
#include "engineauthoringlists.hpp"

#include "../fullscreenpass/fullscreeninputbindingmismatch.hpp"
#include "../fullscreenpass/shaderinputmismatchjson.hpp"
#include "../material/materialphysicalcontractchanged.hpp"
#include "../renderingpass/passshaderdefines.hpp"
#include "../loader/engineresources.hpp"
#include "../renderingpass/frameplanner.hpp"
#include "../renderer/viewfamilyproviderregistry.hpp"
#include "../renderingpass/passdefinitionjsonparser.hpp"
#include "../renderingpass/renderingpassjsonhelpers.hpp"
#include "../renderingpass/rendertargetjsonparser.hpp"
#include "../renderingpass/rendertargetmetadataresolver.hpp"
#include "../renderingpass/rendertargetnameresolver.hpp"
#include "../shader/shaderreflection.hpp"
#include "../vkcore/renderer_config.hpp"
#include "../watch/problemlist.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <tuple>
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
           code == renderConfigLegacyTransactionDirCode ||
           // LP-Va: recompile-time view_select refusals.
           isViewSelectRefusalCode(code) ||
           // VF-1: a secondary view family's passes at different extents.
           code == viewFamilyExtentMismatchCode ||
           // LP-2: the refusals raised while the draw-loop tables are
           // built (the steps are judged against the loaded materials).
           isMaterialDrawLoopRefusalCode(code) ||
           // LC-1: a push-constant range partly over the loop index.
           code == shaderLoopPushOverlapCode ||
           // BS-1b: a feature or preset that is not JSON.
           code == renderDocumentSyntaxCode ||
           // PB-1: a feature whose module this engine started without.
           code == renderFeatureRequiresRestartCode;
}

// LP-0a: a graph refusal that only composing the candidate can raise (a
// render.target_ref naming no composed target, graph.target_ref_unknown@1)
// keeps its code and fields, as the same refusal does when an edit raises it.
std::optional<std::pair<std::string, Json>> graphRefusalOf(
    const std::exception_ptr &error) {
    if (!error) return std::nullopt;
    try {
        std::rethrow_exception(error);
    } catch (const GraphRegistryError &refusal) {
        return std::pair{refusal.code(), refusal.detail()};
    } catch (...) {
    }
    return std::nullopt;
}

// CT-2: a graph.* refusal's details with the root's reference as `document`
// and, judged against `root_bytes` (the root the refusal was raised on), the
// definition its node belongs to. Fields the details already have are kept.
OrderedJson withGraphRefusalDocument(OrderedJson details, std::string_view code,
                                     std::string_view root_bytes,
                                     std::string_view reference) {
    if (!code.starts_with("graph.")) return details;
    if (details.is_null()) details = OrderedJson::object();
    addGraphRefusalSource(details, code,
                          Json::parse(root_bytes, nullptr, false), reference);
    return details;
}

// CT-5: for each definition of the root, the root nodes that use it -- a
// graph.instance (owned when it has an `owner`) or a graph.loop whose branch
// runs it as a body (never owned: its copies stand in the root) -- and
// whether every use is owned, so an op whose placement is owned_instance
// (pose.*, value.morph_weight) may stand in it. A definition nothing uses
// lists none and is owned_instances_only.
OrderedJson graphDefinitionInstances(const Json &root) {
    auto result = OrderedJson::object();
    const auto definitions = root.find("definitions");
    if (definitions == root.end() || !definitions->is_object()) return result;
    for (const auto &[name, body] : definitions->items()) {
        (void)body;
        result[name] = {{"owned_instances_only", true}, {"instances", OrderedJson::array()}};
    }
    const auto use = [&](const std::string &definition, const Json &id, const char *op, bool owned) {
        const auto found = result.find(definition);
        if (found == result.end()) return;
        (*found)["instances"].push_back({{"id", id}, {"op", op}, {"owned", owned}});
        if (!owned) (*found)["owned_instances_only"] = false;
    };
    const auto nodes = root.find("nodes");
    if (nodes == root.end() || !nodes->is_array()) return result;
    for (const auto &node : *nodes) {
        if (!node.is_object() || !node.contains("id")) continue;
        const auto op = node.value("op", std::string{});
        if (op == "graph.instance") {
            const auto definition = node.find("definition");
            if (definition != node.end() && definition->is_string())
                use(definition->get<std::string>(), node.at("id"), "graph.instance", node.contains("owner"));
        } else if (op == "graph.loop") {
            const auto parameters = node.find("parameters");
            if (parameters == node.end() || !parameters->is_object()) continue;
            const auto branches = parameters->find("branches");
            if (branches == parameters->end() || !branches->is_array()) continue;
            std::set<std::string> bodies;
            for (const auto &branch : *branches) {
                const auto body = branch.is_object() ? branch.find("body") : branch.end();
                if (body != branch.end() && body->is_string() && bodies.insert(body->get<std::string>()).second)
                    use(body->get<std::string>(), node.at("id"), "graph.loop", false);
            }
        }
    }
    return result;
}

// Q-14e: the fields a typed render_pipeline_preflight_failed refusal carries,
// added beside the ones a call site already has. Every refusal with that code
// that is built from an exception goes through here, whichever way the
// exception came (a catch in this file, or the renderer's apply result).
OrderedJson withPreflightRefusalFields(OrderedJson details,
                                       const std::exception &error) {
    if (const auto *mismatch =
            dynamic_cast<const FullscreenInputBindingMismatch *>(&error)) {
        // VI-1b: the one shape a reload's refused passes carry too.
        if (details.is_null()) details = OrderedJson::object();
        details["shader_input_mismatch"] =
            shaderInputMismatchJson(mismatch->info());
    }
    if (const auto *define =
            dynamic_cast<const PassShaderDefineError *>(&error)) {
        const auto &info = define->info();
        if (details.is_null()) details = OrderedJson::object();
        details["pass_shader_define"] = OrderedJson{
            {"pass", info.pass},     {"define", info.define},
            {"name", info.name},     {"index", info.index},
            {"reason", info.reason},
        };
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

// RS-1: the restart_reason of a runtime refusal that was only a live
// material's physical rendering contract, or nullopt for any other failure.
std::optional<OrderedJson> physicalContractRestartReason(
    const std::exception_ptr &error) {
    if (!error) return std::nullopt;
    try {
        std::rethrow_exception(error);
    } catch (const MaterialPhysicalContractChanged &change) {
        const auto &info = change.info();
        return OrderedJson{
            {"code", materialPhysicalContractChangedCode},
            {"material", info.material},
            {"material_id", info.material_id},
            {"pass", info.pass},
            {"detail", info.detail},
            {"message", change.what()},
        };
    } catch (...) {
    }
    return std::nullopt;
}

// A readable message for a carried refusal; the fields stay structured.
std::string carriedRefusalMessage(const CodedRefusal &refusal) {
    if (refusal.code == materialRouteUnservedCode) {
        return materialRouteUnservedText(refusal.fields);
    }
    if (refusal.code == renderDocumentSyntaxCode)
        return renderDocumentSyntaxMessage(refusal.fields);
    if (refusal.code == renderFeatureRequiresRestartCode)
        return renderFeatureRequiresRestartText(refusal.fields);
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

// G-4's material_route_default of the root the editor holds. BS-1b: one that
// is not JSON authors none, as the engine on its fallback generation read none.
MaterialRouteDefault heldRouteDefault(std::string_view held_root) {
    const auto held = Json::parse(held_root, nullptr, false);
    if (held.is_discarded()) return readMaterialRouteDefault(Json::object());
    return readMaterialRouteDefault(renderingSectionOrEmpty(held));
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
      // BS-1b: a root that is not JSON (an engine on its fallback generation
      // was refused it) is held as its bytes, to be listed and replaced.
      document_{Json::accept(dependencies_.source_bytes)
                    ? AuthoredRenderConfigDocument::inspect(
                          dependencies_.source_bytes)
                    : AuthoredRenderConfigDocument::unparsed(
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
    // FG-2: a running verify_fold's worker stops its child (it closes the
    // child's job after a grace) and returns; wait for every one.
    fold_verify_cancelled_->store(true);
    for (auto &task : fold_verifies_)
        if (task.runs.valid()) task.runs.wait();
    fold_verifies_.clear();
    // AB-1: a bake finishes what it writes.
    for (auto &task : audio_bakes_)
        if (task.outcome.valid()) task.outcome.wait();
    audio_bakes_.clear();
    lifetime_.reset();
}

void RenderConfigEditorService::loadManagedDocumentBaselines() {
    managed_documents_.clear();
    // BS-1b: a root that is not JSON names no fragment.
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path ||
        !Json::accept(document_.bytes())) {
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
    return documentSetOf(document_);
}

RenderConfigCandidateDocumentSet
RenderConfigEditorService::readDocumentSet() const {
    return documentSetOf(readDocument());
}

RenderConfigCandidateDocumentSet RenderConfigEditorService::documentSetOf(
    const AuthoredRenderConfigDocument &root) const {
    std::vector<RenderConfigCandidateDocument> documents;
    documents.reserve(managed_documents_.size() + 1);
    documents.push_back(RenderConfigCandidateDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .operation = RenderConfigDocumentOperation::replace,
        .expected =
            {RenderConfigDocumentExistence::present,
             root.sourceDigest()},
        .bytes = root.bytes(),
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
    // FR-3: inside edit_batch, the other documents its edits wrote.
    if (batch_)
        for (const auto &document : batch_->documents)
            if (document.operation != RenderConfigDocumentOperation::erase)
                documents.push_back(document);
    return RenderConfigCandidateDocumentSet{
        source_normalized_reference_, std::move(documents)};
}

// FR-4: renderconfigeditor.hpp.
RenderConfigEditorService::HeldDocument
RenderConfigEditorService::heldDocument(const std::string &reference) const {
    if (batch_ && dependencies_.normalize_reference) {
        const auto normalized = dependencies_.normalize_reference(reference);
        for (const auto &document : batch_->documents)
            if (document.normalized_reference == normalized &&
                document.operation != RenderConfigDocumentOperation::erase)
                return {.bytes = document.bytes, .base_digest = document.expected.digest};
    }
    auto bytes = readFileBytes(dependencies_.resolve_document_path(reference));
    auto digest = renderConfigSourceDigest(bytes);
    return {.bytes = std::move(bytes), .base_digest = std::move(digest)};
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
    // CT-2: every graph.* refusal names the root document its node was
    // written in. A caller that held the candidate root has added the fields
    // already; otherwise the root as this editor holds it is read.
    details = withGraphRefusalDocument(std::move(details), code,
                                       document_.bytes(),
                                       dependencies_.source_reference);
    details["code"] = std::move(code);
    details["message"] = std::move(message);
    return {{"status", "rejected"},
            {"committed", false},
            {"error", std::move(details)}};
}

namespace {
// PV-1: a committed result names the published generation's preview state
// (an edit only preview cannot take is published, with preview unavailable).
OrderedJson withPublishedPreview(OrderedJson result, const nlohmann::json &preview) {
    if (!preview.is_null()) result["preview"] = preview;
    return result;
}
} // namespace

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
        // LP-0d: a named refusal keeps its code and fields, `variant` beside
        // them, as the same refusal on the main path does.
        if (auto refusal = splitCodedRefusalMessage(error.what());
            refusal && isCarriedRefusalCode(refusal->code)) {
            for (const auto &[key, value] : refusal->fields.items())
                if (!details.contains(key)) details[key] = value;
            return rejection(refusal->code, carriedRefusalMessage(*refusal),
                             std::move(details));
        }
        if (auto refusal = graphRefusalOf(error.cause())) {
            for (const auto &[key, value] : refusal->second.items())
                if (!details.contains(key)) details[key] = value;
            // CT-2: judged against the candidate's root, not the live one.
            details = withGraphRefusalDocument(
                std::move(details), refusal->first,
                candidate.rootDocument().bytes,
                dependencies_.source_reference);
            return rejection(refusal->first, error.what(), std::move(details));
        }
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
        readDocument().uneditableFeatureEntryCount();
    auto instances = OrderedJson::array();
    const auto root = Json::parse(readDocument().bytes());
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
            // instead of the engine default.  The declaration's own default
            // (the engine's) stays readable as `engine_default`, on every
            // declaration of a materialized feature that has one.
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
                            if (const auto original = declaration.find("default"); original != declaration.end()) {
                                auto engine_default = *original;
                                declaration["engine_default"] = std::move(engine_default);
                            }
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
                     dependencies_.source_reference, readDocument().sourceDigest(), true);
    if (dependencies_.resolve_authoring_context) {
        const auto current = dependencies_.resolve_authoring_context(readDocumentSet());
        if (current.pipeline_preset && current.pipeline_preset_bytes) {
            const auto preset = Json::parse(*current.pipeline_preset_bytes);
            append_instances(preset.at("config").value("features", Json::array()), "preset",
                             current.pipeline_preset->reference, renderConfigSourceDigest(*current.pipeline_preset_bytes),
                             current.pipeline_preset->reference.starts_with("project://"));
        }
    }
    return markRead(OrderedJson{{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(readDocument().sourceDigest())}, {"live", readLive()},
            {"features", readDocument().featureReferences()},
            {"feature_instances", std::move(instances)},
            {"has_uneditable_feature_entries",
             uneditable_entries != 0},
            {"uneditable_feature_entry_count",
             uneditable_entries},
            {"runtime", runtimeJson(dependencies_.runtime_snapshot())}});
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
        // FR-1: the targets the pipeline has to declare for the feature to
        // compose (feature.required_target_missing@1 otherwise). A document
        // that cannot be read lists none; adding it says why.
        encoded["required_targets"] = OrderedJson::array();
        // FR-2: where its passes go, [{pass, anchor, position}], for
        // feature.insertion_anchor_missing@1 to be avoided before the edit.
        encoded["insertion"] = OrderedJson::array();
        try {
            std::string bytes;
            if (entry.reference.starts_with("engine://"))
                bytes = engineResourceOrThrow(entry.reference.substr(9));
            else if (dependencies_.resolve_document_path)
                bytes = readFileBytes(dependencies_.resolve_document_path(entry.reference));
            if (!bytes.empty()) {
                const auto feature = Json::parse(bytes);
                // FR-3: each entry says needs_writer (the feature reads it and
                // someone in the pipeline has to draw it first).
                encoded["required_targets"] = OrderedJson::parse(renderFeatureRequiredTargets(feature, /*with_needs_writer=*/true).dump());
                for (const auto &insertion : renderFeatureInsertions(feature))
                    encoded["insertion"].push_back(OrderedJson{{"pass", insertion.at("pass").get<std::string>()},
                                                               {"anchor", insertion.at("anchor").get<std::string>()},
                                                               {"position", insertion.at("position").get<std::string>()}});
            }
        } catch (const std::exception &) {
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
            // LP-Vb: a graph.loop over view_axis is sized by the providers.
            .view_axis_length = renderGraphViewAxisLengthResolver(),
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
    // RR-1: while the held root is not JSON, an edit on the root the reads
    // answer from (readable_document_) is external_modification, as an edit
    // on a digest from before a reload always was: the disk is not that root.
    // On the held bytes' digest it is render_document_syntax@1 (below).
    if (base_digest != document_.sourceDigest()) {
        return rejection(
            "external_modification",
            "render config base digest no longer matches the engine-owned document",
            {{"expected_source_digest", document_.sourceDigest()},
             {"provided_source_digest", base_digest}});
    }
    // FR-3: inside edit_batch the editor holds the batch's candidate, and the
    // batch compared the disk with what it held before its first edit.
    if (!batch_) try {
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
    // BS-1b: nothing in a root that is not JSON can be edited in place; it is
    // fixed on disk and reloaded, or replaced whole (restore_render_documents).
    return syntaxRejection(document_.bytes(), dependencies_.source_reference,
                           method);
}

std::optional<OrderedJson> RenderConfigEditorService::syntaxRejection(
    std::string_view bytes, std::string_view reference,
    std::string_view method) const {
    const auto fields = renderDocumentSyntax(bytes, reference);
    if (!fields) return std::nullopt;
    auto details = OrderedJson::object();
    details["method"] = std::string{method};
    for (const auto &[key, value] : fields->items()) details[key] = value;
    return rejection(std::string{renderDocumentSyntaxCode},
                     renderDocumentSyntaxMessage(*fields), std::move(details));
}

OrderedJson RenderConfigEditorService::queueRootEdit(
    const std::string &base_digest, const std::string &operation,
    const std::function<std::optional<OrderedJson>(AuthoredRenderConfigDocument &)> &transform,
    std::span<const RenderConfigCandidateDocument> additional_documents,
    std::optional<std::string> base_preset_digest, bool source_only,
    std::optional<std::string> ticket_id_override) {
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
        ticket_id_override ? *ticket_id_override
                           : "render-config-edit-" + std::to_string(next_ticket_++);
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
    // FR-3: inside edit_batch the batch preflights its last candidate once.
    if (!source_only && !batch_) {
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

// --- FR-3: several document edits as one ticket (renderconfigeditor.hpp).

namespace {

using BatchEdit = OrderedJson (RenderConfigEditorService::*)(const Json &);

// The methods edit_batch takes. Each answers with one ticket of
// queueRootEdit or enqueueAuthoredPassTicket and reads the documents it
// rewrites from the editor's own (document_, the managed fragments, the
// authoring context of currentDocumentSet(), FR-4 heldDocument() for a
// project preset or feature), so it builds on what the edits before it in the
// batch made.
const std::vector<std::pair<std::string_view, BatchEdit>> &batchEdits() {
    static const std::vector<std::pair<std::string_view, BatchEdit>> edits{
        {"edit_render_features", &RenderConfigEditorService::editRenderFeatures},
        {"edit_render_targets", &RenderConfigEditorService::editRenderTargets},
        {"add_authored_pass", &RenderConfigEditorService::addAuthoredPass},
        {"remove_authored_pass", &RenderConfigEditorService::removeAuthoredPass},
        {"edit_graph_document", &RenderConfigEditorService::editGraphDocument},
        {"edit_graph_instances", &RenderConfigEditorService::editGraphInstances},
        {"decompose_material_pass", &RenderConfigEditorService::decomposeMaterialPass},
        {"apply_graph_template", &RenderConfigEditorService::applyGraphTemplate},
        {"fold_nodes", &RenderConfigEditorService::foldNodes},
        {"set_material_route_default", &RenderConfigEditorService::setMaterialRouteDefault},
        {"edit_rendering_keys", &RenderConfigEditorService::editRenderingKeys},
        {"edit_frame_node_order", &RenderConfigEditorService::editFrameNodeOrder},
        {"edit_target_planning", &RenderConfigEditorService::editTargetPlanning},
        {"edit_vulkan_plan_pins", &RenderConfigEditorService::editVulkanPlanPins},
        {"put_vulkan_physical_fragment", &RenderConfigEditorService::putVulkanPhysicalFragment},
        {"fork_render_pipeline", &RenderConfigEditorService::forkRenderPipeline},
        {"edit_region_replacement", &RenderConfigEditorService::editRegionReplacement},
        {"add_compute_task", &RenderConfigEditorService::addComputeTask},
        {"edit_ray_callable_records", &RenderConfigEditorService::editRayCallableRecords},
        {"edit_scene_acceleration_structures", &RenderConfigEditorService::editSceneAccelerationStructures},
        // FR-4.
        {"edit_preset_pass", &RenderConfigEditorService::editPresetPass},
        {"edit_compute_task", &RenderConfigEditorService::editComputeTask},
        {"remove_compute_task", &RenderConfigEditorService::removeComputeTask},
    };
    return edits;
}

} // namespace

std::vector<std::string> RenderConfigEditorService::editBatchMethods() {
    std::vector<std::string> methods;
    for (const auto &[method, edit] : batchEdits()) {
        (void)edit;
        methods.emplace_back(method);
    }
    return methods;
}

OrderedJson RenderConfigEditorService::editBatch(const Json &params) {
    constexpr std::string_view method = "edit_batch";
    requireOnly(params, {"base_source_digest", "edits"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    if (!isLowerHexDigest(base_digest))
        throw std::invalid_argument("edit_batch base_source_digest must be 64 lowercase hex characters");
    if (batch_) throw std::invalid_argument("edit_batch cannot be nested");
    const auto edits = params.find("edits");
    if (edits == params.end() || !edits->is_array() || edits->empty())
        throw std::invalid_argument("edit_batch requires a non-empty edits array");
    std::vector<BatchEdit> calls;
    std::vector<std::string> methods;
    for (std::size_t index = 0; index < edits->size(); ++index) {
        const auto &entry = edits->at(index);
        const auto where = "edit_batch edits[" + std::to_string(index) + "]";
        if (!entry.is_object()) throw std::invalid_argument(where + " must be an object {method, params}");
        for (const auto &[key, value] : entry.items()) {
            (void)value;
            if (key != "method" && key != "params")
                throw std::invalid_argument(where + " accepts only method and params: " + key);
        }
        if (!entry.contains("method") || !entry.at("method").is_string())
            throw std::invalid_argument(where + " requires a string method");
        if (!entry.contains("params") || !entry.at("params").is_object())
            throw std::invalid_argument(where + " requires a params object");
        const auto name = entry.at("method").get<std::string>();
        const auto found = std::find_if(batchEdits().begin(), batchEdits().end(),
                                        [&](const auto &candidate) { return candidate.first == name; });
        if (found == batchEdits().end()) {
            std::string listed;
            for (const auto &allowed : editBatchMethods()) listed += (listed.empty() ? "" : ", ") + allowed;
            throw std::invalid_argument(where + " method is not one edit_batch takes: " + name + " (it takes " +
                                        listed + ")");
        }
        if (entry.at("params").contains("base_source_digest"))
            throw std::invalid_argument(where + " params must not carry base_source_digest; the batch's is the only one");
        calls.push_back(found->second);
        methods.push_back(name);
    }

    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(
            gate, base_digest, method, "render config editing is currently disabled"))
        return *rejected;
    // Every managed fragment as the editor holds it, compared with the disk
    // once here: inside the batch the editor holds the batch's candidate.
    for (const auto &[normalized, managed] : managed_documents_) {
        (void)normalized;
        try {
            if (renderConfigSourceDigest(readFileBytes(managed.path)) != managed.digest)
                return rejection("external_modification", "managed render fragment changed outside the editor",
                                 {{"fragment_reference", managed.reference}});
        } catch (const std::exception &error) {
            return rejection("external_modification", error.what(), {{"fragment_reference", managed.reference}});
        }
    }

    // What the batch changes while it runs, put back whichever way it ends.
    struct Restore {
        RenderConfigEditorService &service;
        AuthoredRenderConfigDocument document;
        std::unordered_map<std::string, ManagedDocumentBaseline> managed;
        std::size_t pending;
        ~Restore() {
            service.document_ = std::move(document);
            service.managed_documents_ = std::move(managed);
            service.batch_.reset();
            while (service.pending_.size() > pending) {
                service.results_.erase(service.pending_.back().id);
                service.pending_.pop_back();
            }
        }
    } restore{*this, document_, managed_documents_, pending_.size()};
    batch_.emplace();

    std::vector<Ticket> tickets;
    for (std::size_t index = 0; index < calls.size(); ++index) {
        auto edit_params = edits->at(index).at("params");
        edit_params["base_source_digest"] = document_.sourceDigest();
        OrderedJson answer;
        try {
            answer = (this->*calls[index])(edit_params);
        } catch (const std::invalid_argument &error) {
            throw std::invalid_argument("edit_batch edits[" + std::to_string(index) + "] (" + methods[index] +
                                        "): " + error.what());
        }
        if (answer.value("status", std::string{}) != "accepted") {
            if (answer.contains("error") && answer.at("error").is_object())
                answer["error"]["batch_index"] = index;
            return answer;
        }
        if (pending_.size() <= restore.pending || pending_.back().id != answer.value("ticket", std::string{}))
            throw std::logic_error("edit_batch: " + methods[index] + " was accepted without its ticket");
        auto ticket = std::move(pending_.back());
        pending_.pop_back();
        results_.erase(ticket.id);
        if (!ticket.candidate_documents)
            throw std::logic_error("edit_batch: " + methods[index] + " has no candidate document set");
        // The next edit reads what this one made.
        document_ = ticket.candidate;
        for (const auto &document : ticket.candidate_documents->documents()) {
            if (document.normalized_reference == source_normalized_reference_) continue;
            const auto managed = managed_documents_.find(document.normalized_reference);
            const bool erase = document.operation == RenderConfigDocumentOperation::erase;
            if (managed != managed_documents_.end() ||
                (!erase && hasManagedFragmentMarker(document.bytes) &&
                 isManagedFragmentPath(dependencies_.project_root, document.path))) {
                if (erase) {
                    managed_documents_.erase(document.normalized_reference);
                } else {
                    auto digest = managed != managed_documents_.end() ? managed->second.digest
                                                                      : renderConfigSourceDigest(document.bytes);
                    managed_documents_.insert_or_assign(
                        document.normalized_reference,
                        ManagedDocumentBaseline{.reference = document.reference,
                                                .normalized_reference = document.normalized_reference,
                                                .path = document.path,
                                                .bytes = document.bytes,
                                                .digest = std::move(digest)});
                }
                continue;
            }
            auto &others = batch_->documents;
            const auto existing = std::find_if(others.begin(), others.end(), [&](const auto &entry) {
                return entry.normalized_reference == document.normalized_reference;
            });
            if (existing != others.end()) *existing = document;
            else others.push_back(document);
        }
        tickets.push_back(std::move(ticket));
    }

    // One candidate: every document at what the last edit that carried it
    // made, expected as it was before the first edit that carried it.
    std::map<std::string, RenderConfigDocumentExpectedState> first_expected;
    std::map<std::string, RenderConfigCandidateDocument> latest;
    for (const auto &ticket : tickets)
        for (const auto &document : ticket.candidate_documents->documents()) {
            first_expected.try_emplace(document.normalized_reference, document.expected);
            latest.insert_or_assign(document.normalized_reference, document);
        }
    std::vector<RenderConfigCandidateDocument> documents;
    const auto take = [&](const std::string &normalized) {
        auto found = latest.find(normalized);
        if (found == latest.end()) return;
        auto document = std::move(found->second);
        latest.erase(found);
        document.expected = first_expected.at(normalized);
        if (document.expected.existence == RenderConfigDocumentExistence::missing) {
            // Created by the batch: an erase leaves nothing to write.
            if (document.operation == RenderConfigDocumentOperation::erase) return;
            document.operation = RenderConfigDocumentOperation::create;
        }
        documents.push_back(std::move(document));
    };
    // A document only an earlier edit carried goes first, as a dependency
    // goes before the root that names it; then the last edit's, in its order.
    std::vector<std::string> last_order;
    for (const auto &document : tickets.back().candidate_documents->documents())
        last_order.push_back(document.normalized_reference);
    for (const auto &ticket : tickets)
        for (const auto &document : ticket.candidate_documents->documents())
            if (std::find(last_order.begin(), last_order.end(), document.normalized_reference) == last_order.end())
                take(document.normalized_reference);
    for (const auto &normalized : last_order) take(normalized);
    RenderConfigCandidateDocumentSet candidate_set{source_normalized_reference_, std::move(documents)};

    Ticket merged{
        .id = "render-config-edit-" + std::to_string(next_ticket_++),
        .accepted_gate_epoch = gate.epoch,
        .base_source_digest = base_digest,
        .candidate = tickets.back().candidate,
        .candidate_documents = std::nullopt,
        .operation = std::string{method},
    };
    merged.source_only = true;
    for (auto &ticket : tickets) {
        if (!merged.base_preset_digest) merged.base_preset_digest = ticket.base_preset_digest;
        merged.edits_additional_documents = merged.edits_additional_documents || ticket.edits_additional_documents;
        merged.source_only = merged.source_only && ticket.source_only;
        merged.restart_required = merged.restart_required || ticket.restart_required;
        merged.reload_managed_baselines = merged.reload_managed_baselines || ticket.reload_managed_baselines;
        for (const auto &[key, value] : ticket.result_extras.items())
            if (!merged.result_extras.contains(key)) merged.result_extras[key] = value;
    }
    merged.result_extras["methods"] = methods;
    auto checks = std::move(batch_->checks);
    const bool composes = batch_->composes;

    // Composed and preflighted once, as the editor holds the documents.
    document_ = restore.document;
    managed_documents_ = restore.managed;
    batch_.reset();
    const OrderedJson details{{"participant", "pelican.render_pipeline"}, {"method", std::string{method}}};
    if (!merged.source_only) {
        if (composes && dependencies_.resolve_authoring_context) {
            try {
                const auto composed = dependencies_.resolve_authoring_context(candidate_set);
                for (const auto &check : checks)
                    if (check) check(composed);
            } catch (const GraphRegistryError &error) {
                OrderedJson fields = details;
                const auto named = OrderedJson::parse(error.detail().dump());
                for (const auto &[key, value] : named.items())
                    if (!fields.contains(key)) fields[key] = value;
                return rejection(error.code(), error.what(),
                                 withGraphRefusalDocument(std::move(fields), error.code(),
                                                          candidate_set.rootDocument().bytes,
                                                          dependencies_.source_reference));
            } catch (const std::exception &error) {
                return preflightRejection(error, details);
            }
        }
        if (auto refused = preflightGraphVariants(candidate_set, details)) return *refused;
    }
    merged.candidate_documents = std::move(candidate_set);
    const auto ticket_id = merged.id;
    pending_.push_back(std::move(merged));
    // The batch's ticket stays when the guard trims what the edits queued.
    restore.pending = pending_.size();
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return {{"ticket", ticket_id},
            {"status", "accepted"},
            {"base_source_digest", digestJson(base_digest)},
            {"methods", methods}};
}

OrderedJson RenderConfigEditorService::getRayCallableRecords(const Json &params) const {
    requireOnly(params, {}, "get_ray_callable_records");
    const auto root = Json::parse(readDocument().bytes());
    const auto config = dependencies_.resolve_authoring_context
        ? dependencies_.resolve_authoring_context(readDocumentSet()).config
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
    return markRead(OrderedJson{{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(readDocument().sourceDigest())}, {"live", readLive()}, {"tasks", tasks}});
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
    const auto root = Json::parse(readDocument().bytes());
    Json connections = Json::array();
    const auto config = dependencies_.resolve_authoring_context
        ? dependencies_.resolve_authoring_context(readDocumentSet()).config
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
    return markRead(OrderedJson{{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(readDocument().sourceDigest())}, {"live", readLive()},
            {"task_connections", connections},
            {"triangle_geometry", geometrySpaceDocument(config, dependencies_).info},
            {"procedural_geometry", geometrySpaceDocument(config, dependencies_, true).info},
            {"definitions", renderingSectionOrEmpty(root).value("scene_acceleration_structures", Json::array())}});
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
        dependencies_.resolve_authoring_context(readDocumentSet());
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
            editable_root = OrderedJson::parse(readDocument().bytes());
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
        {"source_digest", digestJson(readDocument().sourceDigest())}, {"live", readLive()},
        {"published_generation", runtime.published_generation},
        // G-1: what the running pipeline enabled. A scene-class target is
        // R16G16B16A16_SFLOAT only with "hdr" among them, which decides the
        // formats a new G-buffer's targets are made with.
        {"enabled_feature_names", runtime.enabled_feature_names},
        {"config_kind",
         resolved.pipeline_preset ? "preset" : "direct"},
        {"graphs", std::move(graphs)},
        {"managed_fragments", std::move(fragments)},
        // FR-1: what a rendering.snapshots entry's `after` may name here
        // (snapshot.after_unknown@1 otherwise).
        {"snapshot_after_candidates", renderSnapshotAfterCandidates(resolved.config)},
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
        renderTargetOrigins(resolved, readDocument().bytes(), fragment_targets);
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
        // CT-5: how a class with an argument is written, as
        // resolveRenderTargetFormatClasses reads it: "explicit(<format>)",
        // the format one of `formats`.
        {"format_class_patterns",
         OrderedJson::array({OrderedJson{
             {"name", "explicit"},
             {"form", "explicit(<format>)"},
             {"argument_choices_from",
              {{"rpc", "get_render_authoring_context"}, {"key", "render_target_vocabulary.formats"}}}}})},
    };
    // EL-4: each format's aspect, from its Vulkan format traits; `formats`
    // stays the list of names.
    auto format_aspects = OrderedJson::object();
    for (const auto &[name, format] : imageFormats())
        format_aspects[name] = std::string{imageFormatAspectName(format)};
    result["render_target_vocabulary"]["format_aspects"] = std::move(format_aspects);
    // CT-4: the names a pass's view_family and canonical_anchor take (the
    // catalog's choices_from): the main family and each registered
    // provider's, and the anchors featurecompose places passes at.
    auto view_families = OrderedJson::array({std::string{mainRenderViewFamilyId}});
    for (const auto &provider : renderViewFamilyProviderRegistry().providers())
        view_families.push_back(provider.family_id);
    result["view_families"] = std::move(view_families);
    result["canonical_anchors"] = canonicalRenderPassAnchors();
    // CT-5: the names a ray_query's acceleration_structure takes: "scene" and
    // each composed scene_acceleration_structures entry
    // (vulkanrendercompilerprogram.cpp refuses any other).
    auto acceleration_structures = OrderedJson::array({"scene"});
    if (const auto declared = resolved.config.find("scene_acceleration_structures");
        declared != resolved.config.end() && declared->is_array())
        for (const auto &entry : *declared)
            if (entry.is_object() && entry.contains("name") && entry.at("name").is_string())
                acceleration_structures.push_back(entry.at("name"));
    result["acceleration_structures"] = std::move(acceleration_structures);
    // EL-1: the names a draw_loop branch's `when` and draw.variant take, from
    // what the draw-loop tables judge (MaterialDrawSubject): `id` is the
    // matching key `when.material` compares, `tags` what `when.tags` / `where`
    // test, `variants` the names draw.variant and has_variant accept. A
    // built-in engine material is the one named under
    // builtinMaterialNamePrefix (describeUnservedMaterial reads it so too).
    auto materials = OrderedJson::array();
    std::set<std::string> material_tags;
    if (dependencies_.draw_loop_materials) {
        for (const auto &subject : dependencies_.draw_loop_materials()) {
            material_tags.insert(subject.tags.begin(), subject.tags.end());
            materials.push_back({
                {"id", subject.key},
                {"name", subject.name},
                {"tags", subject.tags},
                {"variants", subject.variants},
                {"builtin", subject.name.starts_with(builtinMaterialNamePrefix)},
            });
        }
    }
    result["materials"] = std::move(materials);
    result["material_tags"] = material_tags;
    // EL-1: the provider names edit_region_replacement may give. The registry
    // holds no region a provider accepts, so none is listed.
    auto region_providers = OrderedJson::array();
    if (dependencies_.region_providers) {
        for (const auto &provider : dependencies_.region_providers()) {
            region_providers.push_back({
                {"name", provider.name},
                {"builtin", provider.engine_owned},
                {"provider_version", provider.provider_version},
            });
        }
    }
    result["region_providers"] = std::move(region_providers);
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
    return markRead(std::move(result));
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
    const auto context = dependencies_.resolve_authoring_context(readDocumentSet());
    // MG-3b: the tasks a render.compute node made. GL-2a: the composition
    // says which node made each task (RenderPassProvenance::graph_origin),
    // and where that node is authored, so the editor no longer expands the
    // root again to find them. A loop's copy is not listed as a node's task
    // here (as before GL-2a); its origin is in get_frame_plan.
    auto tasks = OrderedJson::array();
    for (const auto &task : context.config.value("compute_tasks", Json::array())) {
        const auto name = task.at("name").get<std::string>();
        const auto provenance = std::find_if(context.pass_provenance.begin(), context.pass_provenance.end(),
            [&](const auto &entry) { return entry.name == name; });
        const bool from_feature = provenance != context.pass_provenance.end() &&
            provenance->source == RenderPipelineProvenanceSource::feature;
        const GraphNodeOrigin *graph_node = !from_feature && provenance != context.pass_provenance.end() &&
                provenance->graph_origin && provenance->graph_origin->loop.empty()
            ? &*provenance->graph_origin
            : nullptr;
        const bool from_graph = graph_node != nullptr;
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
            const auto &source = *graph_node;
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
    return markRead(OrderedJson{{"source_digest", digestJson(readDocument().sourceDigest())}, {"live", readLive()}, {"tasks", std::move(tasks)},
            {"resources", std::move(resources)}});
}

OrderedJson RenderConfigEditorService::getFrameNodeOrder(const Json &params) const {
    requireOnly(params, {}, "get_frame_node_order");
    return markRead(OrderedJson{{"source_digest", digestJson(readDocument().sourceDigest())}, {"live", readLive()},
            {"published_generation", dependencies_.runtime_snapshot().published_generation},
            {"orders", renderingSectionOrEmpty(Json::parse(readDocument().bytes()))
                           .value("frame_node_order", Json::object())}});
}

OrderedJson RenderConfigEditorService::getGraphInstances(const Json &params) const {
    requireOnly(params, {}, "get_graph_instances");
    const auto root = Json::parse(readDocument().bytes());
    // MG-2b: the document is always a pelican.graph root (anything else is
    // refused as render_config.legacy_root@1 before it becomes document_).
    auto instances = Json::array();
    auto nodes = root.at("nodes");
    for (const auto &node : nodes)
        if (node.at("op") == "graph.instance") instances.push_back(node);
    return markRead(OrderedJson{{"source_reference", dependencies_.source_reference},
        // N-8: what the root document is. Since MG-2b it is always
        // "pelican.graph"; the field stays because Studio reads it.
        {"root_schema", root.value("schema", std::string{})},
        {"source_digest", digestJson(readDocument().sourceDigest())}, {"live", readLive()},
        {"instances", std::move(instances)},
        // The whole root array too: an editor that connects an input has to
        // name a producer that is already there, and has to avoid an id that
        // is already taken.
        {"nodes", std::move(nodes)},
        {"definitions", root.value("definitions", Json::object())},
        // CT-5: who uses each definition (graphDefinitionInstances).
        {"definition_instances", graphDefinitionInstances(root)},
        // N-6: the editor's own placement for the root scope. A definition's
        // travels inside its body, so this is the one that needs naming, and
        // reporting it here is what keeps the canvas from needing a read of
        // its own for something the document already carries.
        {"layout", root.value("layout", Json::object())}});
}

OrderedJson RenderConfigEditorService::editGraphInstances(const Json &params) {
    constexpr auto method = "edit_graph_instances";
    requireOnly(params, {"base_source_digest", "attach", "detach", "material_route_default"}, method);
    const auto digest = requireString(params, "base_source_digest", method);
    const auto attached = params.value("attach", Json::array());
    const auto detached = params.value("detach", Json::array());
    // RS-2: material_route_default rides the same ticket, as on
    // edit_graph_document.
    return queueGraphRootEdit(params, digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            // CT-5: the root a refusal is judged against, as on
            // edit_graph_document: the edited one once the edit applied.
            Json judged;
            try {
                judged = Json::parse(candidate.bytes());
                const auto edited = Pelican::editGraphInstances(judged, attached, detached);
                judged = edited;
                (void)partitionGraphDocument(edited);
                candidate = candidate.withRootValue("nodes", edited.at("nodes").dump(2));
                return std::nullopt;
            } catch (const GraphRegistryError &error) {
                // CT-5: a named graph refusal keeps its code and fields, with
                // CT-2's document, as edit_graph_document's does.
                const auto named = graphRefusalInDocument(error, judged, dependencies_.source_reference);
                return rejection(named.code(), named.what(), OrderedJson::parse(named.detail().dump()));
            } catch (const std::exception &error) {
                return rejection("invalid_graph_attachment", error.what());
            }
        });
}

OrderedJson RenderConfigEditorService::editGraphDocument(const Json &params) {
    constexpr auto method = "edit_graph_document";
    requireOnly(params, {"base_source_digest", "ops", "material_route_default"}, method);
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
    // uses, so two edits in the same frame cannot both win. RS-2: the
    // project's material_route_default may travel in the same candidate, as
    // it does on add_authored_pass (the palette's forward material pass needs
    // it on a pipeline that draws nothing deferred).
    return queueGraphRootEdit(params, digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            // CT-2: the root a refusal is judged against -- the edited one
            // once the ops applied, so a node they added is found in it.
            Json judged;
            try {
                const auto source = Json::parse(candidate.bytes());
                judged = source;
                const auto edited = Pelican::editGraphDocument(source, ops);
                judged = edited;
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
                // DO-1: set_outputs may replace the root's public outputs.
                if (edited.value("outputs", Json::object()) !=
                    source.value("outputs", Json::object()))
                    candidate = candidate.withRootValue(
                        "outputs", edited.value("outputs", Json::object()).dump(2));
                // And the root scope's own placement, on the same terms.
                if (edited.value("layout", Json::object()) !=
                    source.value("layout", Json::object()))
                    candidate = candidate.withRootValue(
                        "layout", edited.value("layout", Json::object()).dump(2));
                return std::nullopt;
            } catch (const GraphRegistryError &error) {
                const auto named = graphRefusalInDocument(
                    error, judged, dependencies_.source_reference);
                return rejection(named.code(), named.what(),
                                 OrderedJson::parse(named.detail().dump()));
            } catch (const std::exception &error) {
                return rejection("invalid_graph_document", error.what());
            }
        }, layout_only);
}

// --- FG-1: fold groups (graphfold.hpp).

OrderedJson RenderConfigEditorService::getFoldGroups(const Json &params) const {
    requireOnly(params, {}, "get_fold_groups");
    const auto root = Json::parse(readDocument().bytes());
    auto groups = OrderedJson::array();
    for (const auto &report : reportGraphFoldGroups(root, parseGraphFoldGroups(root)))
        groups.push_back(graphFoldGroupReportToJson(report));
    return markRead(OrderedJson{{"source_digest", digestJson(readDocument().sourceDigest())}, {"live", readLive()}, {"fold_groups", std::move(groups)}});
}

OrderedJson RenderConfigEditorService::queueFoldGroupEdit(
    const std::string &method, const std::string &base_digest, const std::string &group,
    const std::function<OrderedJson(OrderedJson current)> &edit,
    std::optional<std::string> ticket_id) {
    return queueRootEdit(base_digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            const auto root = OrderedJson::parse(candidate.bytes());
            auto groups = root.contains("fold_groups") ? root.at("fold_groups") : OrderedJson::object();
            if (!groups.is_object())
                throw std::invalid_argument(method + ": the root's fold_groups is not an object");
            const auto found = groups.find(group);
            auto next = edit(found != groups.end() ? *found : OrderedJson());
            if (next.is_null()) groups.erase(group);
            else groups[group] = std::move(next);
            // The root as the composition will read it. A shape error is the
            // request's (invalid params); a named one keeps its code.
            auto judged = Json::parse(candidate.bytes());
            judged["fold_groups"] = Json::parse(groups.dump());
            try {
                (void)parseGraphFoldGroups(judged);
            } catch (const GraphRegistryError &error) {
                return rejection(error.code(), error.what(), OrderedJson::parse(error.detail().dump()));
            }
            candidate = groups.empty() ? candidate.withoutRootValue("fold_groups")
                                       : candidate.withRootValue("fold_groups", groups.dump(2));
            return std::nullopt;
        },
        {}, std::nullopt, false, std::move(ticket_id));
}

OrderedJson RenderConfigEditorService::foldNodes(const Json &params) {
    const std::string method = "fold_nodes";
    requireOnly(params, {"base_source_digest", "group", "nodes", "folded"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto group = requireString(params, "group", method);
    if (group.empty()) throw std::invalid_argument("fold_nodes group must not be empty");
    const auto folded = params.find("folded");
    if (folded == params.end())
        throw std::invalid_argument("fold_nodes requires folded (an object {kind, decisions?}, or null)");
    if (folded->is_null()) {
        if (params.contains("nodes"))
            throw std::invalid_argument("fold_nodes takes no nodes when folded is null");
        return queueFoldGroupEdit(method, base_digest, group, [](OrderedJson) { return OrderedJson(); });
    }
    if (!folded->is_object())
        throw std::invalid_argument("fold_nodes folded must be an object {kind, decisions?}, or null");
    if (folded->contains("verified"))
        throw std::invalid_argument(
            "fold_nodes may not write folded.verified; verify_fold writes it when the folded form matches");
    const auto nodes = params.find("nodes");
    if (nodes == params.end() || !nodes->is_array() || nodes->empty())
        throw std::invalid_argument("fold_nodes requires nodes, a non-empty array of root node ids");
    // Every node is one of the root's (the document the CAS names).
    std::set<std::string> root_ids;
    for (const auto &node : Json::parse(document_.bytes()).value("nodes", Json::array()))
        if (node.is_object() && node.contains("id") && node.at("id").is_string())
            root_ids.insert(node.at("id").get<std::string>());
    for (const auto &node : *nodes)
        if (!node.is_string() || !root_ids.contains(node.get<std::string>()))
            throw std::invalid_argument("fold_nodes nodes must name root nodes; the root has no node " +
                                        node.dump());
    OrderedJson entry{{"nodes", OrderedJson::parse(nodes->dump())},
                      {"folded", OrderedJson::parse(folded->dump())}};
    return queueFoldGroupEdit(method, base_digest, group, [&](OrderedJson) { return entry; });
}

// --- FG-2: verify_fold (graphfoldverify.hpp).

namespace {

// An optional non-negative integer field of a verify_fold request.
std::uint64_t verifyFoldCount(const Json &params, const char *field, std::uint64_t fallback,
                              std::uint64_t minimum, std::uint64_t maximum) {
    const auto found = params.find(field);
    if (found == params.end()) return fallback;
    if (!found->is_number_unsigned() && !(found->is_number_integer() && found->get<std::int64_t>() >= 0))
        throw std::invalid_argument(std::string{"verify_fold "} + field + " must be a non-negative integer");
    const auto value = found->get<std::uint64_t>();
    if (value < minimum || value > maximum)
        throw std::invalid_argument(std::string{"verify_fold "} + field + " must be from " +
                                    std::to_string(minimum) + " to " + std::to_string(maximum));
    return value;
}

} // namespace

OrderedJson RenderConfigEditorService::verifyFold(const Json &params) {
    const std::string method = "verify_fold";
    requireOnly(params, {"base_source_digest", "group", "frames", "warmup_seconds", "tolerance"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    if (!isLowerHexDigest(base_digest))
        throw std::invalid_argument("verify_fold base_source_digest must be 64 lowercase hex characters");
    const auto group = requireString(params, "group", method);
    const auto frames = static_cast<std::uint32_t>(verifyFoldCount(params, "frames", 3, 1, 600));
    const auto tolerance = verifyFoldCount(params, "tolerance", 0, 0,
                                           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    double warmup_seconds = 30.0;
    if (const auto found = params.find("warmup_seconds"); found != params.end()) {
        if (!found->is_number() || !std::isfinite(found->get<double>()) || found->get<double>() < 0.0 ||
            found->get<double>() > 3600.0)
            throw std::invalid_argument("verify_fold warmup_seconds must be a number from 0 to 3600");
        warmup_seconds = found->get<double>();
    }

    // The children are the engine's own executable; a launch that may not
    // start them says why, before anything else is judged.
    if (!dependencies_.verify_fold)
        return rejection(std::string{foldVerifyUnavailableCode},
                         "this engine cannot start the runs verify_fold draws", {{"reason", "no_runner"}});
    if (dependencies_.verify_fold_unavailable) {
        if (auto reason = dependencies_.verify_fold_unavailable())
            return rejection(std::string{foldVerifyUnavailableCode},
                             "verify_fold does not start its runs in a " + *reason + " launch",
                             {{"reason", *reason}});
    }
    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(gate, base_digest, method,
                                                    "render config editing is currently disabled"))
        return *rejected;

    const auto root = Json::parse(document_.bytes());
    const auto groups = parseGraphFoldGroups(root);
    const auto declared = std::find_if(groups.begin(), groups.end(),
                                       [&](const GraphFoldGroup &entry) { return entry.group == group; });
    if (declared == groups.end())
        throw std::invalid_argument("verify_fold group '" + group + "' is not a fold group of the root");
    for (const auto &task : fold_verifies_) {
        if (task.group == group)
            return rejection(std::string{foldVerifyBusyCode},
                             "a verify_fold of the fold group '" + group + "' is running",
                             {{"group", group}, {"ticket", task.ticket}});
    }

    // The two forms of the root at base_source_digest: without the group's
    // mark (FG-1 runs it open) and with its mark on the digest of now (FG-1
    // applies its decisions). Nothing else changes.
    const auto digest = graphFoldGroupDigest(root, declared->nodes);
    const auto authored = OrderedJson::parse(document_.bytes()).at("fold_groups");
    auto open_groups = authored;
    open_groups[group]["folded"].erase("verified");
    auto decided_groups = authored;
    decided_groups[group]["folded"]["verified"] = {{"digest", digest}, {"at", foldVerifyUtcNow()}, {"frames", frames}};
    FoldVerifyJob job{
        .group = group,
        .project_root = dependencies_.project_root,
        .root_path = dependencies_.source_path,
        .open_root_bytes = document_.withRootValue("fold_groups", open_groups.dump(2)).bytes(),
        .decided_root_bytes = document_.withRootValue("fold_groups", decided_groups.dump(2)).bytes(),
        .documents = {},
        .frames = frames,
        .warmup_seconds = warmup_seconds,
        .tolerance = tolerance,
    };
    for (const auto &[key, managed] : managed_documents_) {
        (void)key;
        job.documents.push_back({.path = managed.path, .bytes = managed.bytes});
    }

    const auto ticket_id = "render-config-edit-" + std::to_string(next_ticket_++);
    fold_verifies_.push_back(FoldVerifyTask{
        .ticket = ticket_id,
        .group = group,
        .base_source_digest = base_digest,
        .digest = digest,
        .frames = frames,
        .tolerance = tolerance,
        // Its own thread: the frame and the RPCs go on while it draws.
        .runs = std::async(std::launch::async,
                           [runner = dependencies_.verify_fold, job = std::move(job),
                            cancelled = fold_verify_cancelled_] { return runner(job, *cancelled); }),
    });
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "running"}, {"operation", method}, {"group", group}};
    return {{"ticket", ticket_id}, {"status", "accepted"}, {"base_source_digest", digestJson(base_digest)}};
}

void RenderConfigEditorService::harvestFoldVerifies() noexcept {
    for (auto task = fold_verifies_.begin(); task != fold_verifies_.end();) {
        if (task->runs.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            ++task;
            continue;
        }
        auto done = std::move(*task);
        task = fold_verifies_.erase(task);
        const auto finish = [this, &done](OrderedJson result) {
            if (!result.contains("ticket")) result["ticket"] = done.ticket;
            results_[done.ticket] = result;
            completed_.push_back(std::move(result));
        };
        try {
            FoldVerifyRuns runs;
            try {
                runs = done.runs.get();
            } catch (const std::exception &error) {
                runs.setup_failure = error.what();
            }
            if (runs.cancelled) {
                finish(rejection(std::string{foldVerifyUnavailableCode}, "the engine is shutting down",
                                 {{"ticket", done.ticket}, {"reason", "shutdown"}}));
                continue;
            }
            const auto verdict = judgeFoldVerifyRuns(done.group, runs, done.tolerance);
            if (!verdict.passed) {
                auto fields = verdict.fields;
                fields["ticket"] = done.ticket;
                finish(rejection(verdict.code, verdict.message, std::move(fields)));
                continue;
            }
            // The mark, on the document the runs drew: queueRootEdit's CAS
            // refuses it as external_modification when the root moved on.
            const OrderedJson verified{
                {"digest", done.digest}, {"at", foldVerifyUtcNow()}, {"frames", done.frames}};
            auto queued = queueFoldGroupEdit(
                "verify_fold", done.base_source_digest, done.group,
                [&](OrderedJson current) {
                    if (!current.is_object() || !current.contains("folded"))
                        throw std::invalid_argument("verify_fold: the root no longer declares the fold group " +
                                                    done.group);
                    current["folded"]["verified"] = verified;
                    return current;
                },
                done.ticket);
            if (queued.value("status", std::string{}) != "accepted") {
                if (queued.contains("error")) queued["error"]["ticket"] = done.ticket;
                finish(std::move(queued));
                continue;
            }
            // Ahead of the tickets already queued, which were accepted
            // against the same document and would otherwise move it first.
            auto ticket = std::move(pending_.back());
            pending_.pop_back();
            ticket.result_extras = {{"group", done.group}, {"verified", verified}};
            pending_.insert(pending_.begin(), std::move(ticket));
        } catch (const std::exception &error) {
            finish(preflightRejection(error, {{"ticket", done.ticket}, {"group", done.group}}));
        } catch (...) {
            finish(rejection("render_pipeline_preflight_failed", "unknown verify_fold failure",
                             {{"ticket", done.ticket}, {"group", done.group}}));
        }
    }
}

namespace {

bool declaresRenderPass(const OrderedJson &owner, const std::string &pass) {
    const auto families = owner.find("rendering_passes");
    if (families == owner.end() || !families->is_array()) return false;
    for (const auto &family : *families) {
        const auto passes = family.find("passes");
        if (passes == family.end() || !passes->is_array()) continue;
        for (const auto &declaration : *passes)
            if (declaration.is_object() && declaration.value("name", std::string{}) == pass) return true;
    }
    return false;
}

} // namespace

OrderedJson RenderConfigEditorService::decomposeMaterialPass(const Json &params) {
    constexpr auto method = "decompose_material_pass";
    requireOnly(params, {"base_source_digest", "base_preset_digest", "pass"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto pass = requireString(params, "pass", method);
    std::optional<std::string> preset_digest;
    if (params.contains("base_preset_digest")) {
        preset_digest = requireString(params, "base_preset_digest", method);
        if (!isLowerHexDigest(*preset_digest))
            throw std::invalid_argument("decompose_material_pass base_preset_digest must be 64 lowercase hex characters");
    }
    if (!isLowerHexDigest(base_digest))
        throw std::invalid_argument("decompose_material_pass base_source_digest must be 64 lowercase hex characters");
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "decompose_material_pass requires the project document resolver");
    try {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        // The composed targets decide the image type a new render.target_ref
        // declares (materialpassdecompose.hpp).
        const auto composed_target = [&current](const std::string &name) -> const Json * {
            const auto targets = current.config.find("render_targets");
            if (targets == current.config.end() || !targets->is_array()) return nullptr;
            for (const auto &target : *targets)
                if (target.is_object() && target.value("name", std::string{}) == name) return &target;
            return nullptr;
        };
        const auto refused = [&](const MaterialPassDecomposeError &error) {
            return rejection(error.code(), error.what(), error.fields());
        };
        // A name neither the rendering section nor the preset declares.
        const auto undeclared = [&](const OrderedJson &root) {
            const auto graph_pass = [&] {
                const auto nodes = root.find("nodes");
                if (nodes == root.end() || !nodes->is_array()) return false;
                return std::any_of(nodes->begin(), nodes->end(), [&](const OrderedJson &node) {
                    return node.is_object() && node.value("id", std::string{}) == pass &&
                           node.value("op", std::string{}) == "render.material_pass";
                });
            }();
            const auto provenance = std::find_if(current.pass_provenance.begin(), current.pass_provenance.end(),
                [&](const RenderPassProvenance &entry) { return entry.name == pass; });
            OrderedJson fields{{"pass", pass}};
            std::string message;
            if (graph_pass) {
                fields["reason"] = "graph_pass";
                message = "it is already a render.material_pass of the graph";
            } else if (provenance != current.pass_provenance.end() &&
                       provenance->source == RenderPipelineProvenanceSource::feature) {
                fields["reason"] = "feature_pass";
                fields["feature"] = provenance->provider_feature;
                message = "it is a pass of the feature " + provenance->provider_feature +
                          "; replace it with the feature's pass_replacements";
            } else {
                fields["reason"] = "not_found";
                message = "neither the rendering section nor the preset declares it";
            }
            return rejection(std::string{materialPassNotDecomposableCode},
                             "Material pass " + pass + " cannot be decomposed: " + message, std::move(fields));
        };
        const auto summary = [&](const MaterialPassDecomposition &decomposition) {
            return OrderedJson{{"pass", pass},
                               {"family", decomposition.family},
                               {"insert", decomposition.insert},
                               {"loop", decomposition.loop},
                               {"definition", decomposition.definition},
                               {"target_refs", decomposition.target_refs},
                               {"added_target_refs", decomposition.added_target_refs}};
        };

        if (!current.pipeline_preset) {
            // A direct root: its rendering section and its graph, one document.
            if (preset_digest)
                throw std::invalid_argument(
                    "decompose_material_pass: a direct root selects no preset; omit base_preset_digest");
            OrderedJson decomposed;
            auto result = queueRootEdit(base_digest, method,
                [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                    auto root = OrderedJson::parse(candidate.bytes());
                    auto &section = renderingSectionOf(root);
                    if (!declaresRenderPass(section, pass)) return undeclared(root);
                    try {
                        decomposed = summary(::Pelican::decomposeMaterialPass(section, root, pass, composed_target));
                    } catch (const MaterialPassDecomposeError &error) {
                        return refused(error);
                    } catch (const GraphRegistryError &error) {
                        return rejection(error.code(), error.what(), OrderedJson::parse(error.detail().dump()));
                    }
                    candidate = candidate.withSectionValue("rendering_passes", section.at("rendering_passes").dump(2))
                                    .withRootValue("nodes", root.at("nodes").dump(2))
                                    .withRootValue("definitions", root.at("definitions").dump(2));
                    return std::nullopt;
                });
            if (result.at("status") == "accepted") result["decomposed"] = std::move(decomposed);
            return result;
        }

        // A preset root: the pass lives in the preset, which must be the
        // project's own (forked) one; the graph lives in the root. Both
        // digests are compared before the pass is looked at, as the direct
        // root's is (queueRootEdit compares it again, with the gate).
        if (base_digest != document_.sourceDigest())
            return rejection("external_modification",
                             "render config base digest no longer matches the engine-owned document",
                             {{"expected_source_digest", document_.sourceDigest()},
                              {"provided_source_digest", base_digest}});
        const auto current_preset_digest =
            current.pipeline_preset_bytes ? renderConfigSourceDigest(*current.pipeline_preset_bytes) : std::string{};
        if (preset_digest && *preset_digest != current_preset_digest)
            return rejection("external_modification", "The selected preset changed since it was read",
                             {{"expected_preset_digest", current_preset_digest},
                              {"provided_preset_digest", *preset_digest}});
        auto root = OrderedJson::parse(document_.bytes());
        const auto &reference = current.pipeline_preset->reference;
        if (!current.pipeline_preset_bytes) return undeclared(root);
        auto preset = OrderedJson::parse(*current.pipeline_preset_bytes);
        if (!preset.contains("config") || !declaresRenderPass(preset.at("config"), pass)) return undeclared(root);
        if (!reference.starts_with("project://"))
            return rejection("project_preset_required",
                             "Fork the selected preset into the project before decomposing its passes",
                             {{"pass", pass}, {"preset", reference}});
        if (!preset_digest)
            throw std::invalid_argument(
                "decompose_material_pass requires base_preset_digest for a pass of the project preset");
        OrderedJson decomposed;
        try {
            decomposed = summary(::Pelican::decomposeMaterialPass(preset.at("config"), root, pass, composed_target));
        } catch (const MaterialPassDecomposeError &error) {
            return refused(error);
        } catch (const GraphRegistryError &error) {
            return rejection(error.code(), error.what(), OrderedJson::parse(error.detail().dump()));
        }
        decomposed["preset_reference"] = reference;
        const std::array documents{RenderConfigCandidateDocument{
            .reference = reference,
            .normalized_reference = dependencies_.normalize_reference(reference),
            .path = dependencies_.resolve_document_path(reference),
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present, *preset_digest},
            .bytes = preset.dump(2),
        }};
        // The root is document_ itself (queueRootEdit compares the digest
        // first), so the graph computed above is the candidate's.
        auto result = queueRootEdit(base_digest, method,
            [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                candidate = candidate.withRootValue("nodes", root.at("nodes").dump(2))
                                .withRootValue("definitions", root.at("definitions").dump(2));
                return std::nullopt;
            }, documents, preset_digest);
        if (result.at("status") == "accepted") result["decomposed"] = std::move(decomposed);
        return result;
    } catch (const RenderConfigSectionUnlocated &error) {
        return rejection("render_config_section_unlocated", error.what(), {{"method", method}});
    } catch (const std::invalid_argument &) {
        throw;
    } catch (const std::exception &error) {
        return preflightRejection(error);
    }
}

namespace {

// GT-1: the engine resource ids of the shipped graph templates
// ("graphs/<name>.json"), in the registry's order.
std::vector<std::string> graphTemplateResourceIds() {
    std::vector<std::string> ids;
    for (const auto id : registeredEngineResourceIds())
        if (id.starts_with("graphs/") && id.ends_with(".json")) ids.emplace_back(id);
    return ids;
}

// "graphs/shadow_cascade_loop.json" -> "Shadow cascade loop".
std::string graphTemplateTitleFromFile(std::string_view id) {
    auto name = std::string{id.substr(id.rfind('/') + 1)};
    name.resize(name.size() - std::string_view{".json"}.size());
    std::replace(name.begin(), name.end(), '_', ' ');
    if (!name.empty() && name.front() >= 'a' && name.front() <= 'z')
        name.front() = static_cast<char>(name.front() - 'a' + 'A');
    return name;
}

} // namespace

OrderedJson RenderConfigEditorService::getGraphTemplates(const Json &params) const {
    requireOnly(params, {}, "get_graph_templates");
    auto templates = OrderedJson::array();
    for (const auto &id : graphTemplateResourceIds()) {
        auto document = OrderedJson::parse(engineResourceOrThrow(id));
        OrderedJson entry{{"reference", "engine://" + id}};
        const auto title = document.find("title");
        entry["title"] = title != document.end() && title->is_string() ? title->get<std::string>()
                                                                        : graphTemplateTitleFromFile(id);
        if (const auto description = document.find("description");
            description != document.end() && description->is_string())
            entry["description"] = *description;
        entry["decomposes"] = document.value("decomposes", OrderedJson::object());
        entry["document"] = std::move(document);
        templates.push_back(std::move(entry));
    }
    return {{"templates", std::move(templates)}};
}

OrderedJson RenderConfigEditorService::applyGraphTemplate(const Json &params) {
    constexpr auto method = "apply_graph_template";
    requireOnly(params, {"base_source_digest", "base_preset_digest", "template"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    if (!isLowerHexDigest(base_digest))
        throw std::invalid_argument("apply_graph_template base_source_digest must be 64 lowercase hex characters");
    std::optional<std::string> preset_digest;
    if (params.contains("base_preset_digest")) {
        preset_digest = requireString(params, "base_preset_digest", method);
        if (!isLowerHexDigest(*preset_digest))
            throw std::invalid_argument("apply_graph_template base_preset_digest must be 64 lowercase hex characters");
    }
    const auto reference = requireString(params, "template", method);
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "apply_graph_template requires the project document resolver",
                         {{"method", method}});
    const auto templates = graphTemplateResourceIds();
    constexpr std::string_view engine_scheme = "engine://";
    if (!reference.starts_with(engine_scheme) ||
        std::find(templates.begin(), templates.end(), reference.substr(engine_scheme.size())) == templates.end())
        return rejection(std::string{graphTemplateUnknownCode},
                         "Graph template " + reference + " is not one the engine ships (engine://graphs/<name>.json)",
                         {{"template", reference}});
    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(gate, base_digest, method,
                                                    "render config editing is currently disabled"))
        return *rejected;

    // A refusal of the template's own steps, with the template it was about.
    const auto refused = [&](const std::string &code, const std::string &message, OrderedJson fields) {
        fields["template"] = reference;
        return rejection(code, message, std::move(fields));
    };
    try {
        const auto graph_template = OrderedJson::parse(engineResourceOrThrow(reference.substr(engine_scheme.size())));
        GraphTemplateDecomposes decomposes;
        try {
            decomposes = readGraphTemplateDecomposes(graph_template);
        } catch (const GraphTemplateError &error) {
            return refused(error.code(), error.what(), error.fields());
        }
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (!current.pipeline_preset && preset_digest)
            throw std::invalid_argument("apply_graph_template: a direct root selects no preset; omit base_preset_digest");
        std::optional<OrderedJson> preset;
        if (current.pipeline_preset) {
            const auto current_preset_digest = current.pipeline_preset_bytes
                                                   ? renderConfigSourceDigest(*current.pipeline_preset_bytes)
                                                   : std::string{};
            if (preset_digest && *preset_digest != current_preset_digest)
                return rejection("external_modification", "The selected preset changed since it was read",
                                 {{"method", method},
                                  {"expected_preset_digest", current_preset_digest},
                                  {"provided_preset_digest", *preset_digest}});
            if (current.pipeline_preset_bytes) preset = OrderedJson::parse(*current.pipeline_preset_bytes);
        }

        // Where the composition reads the feature: the rendering section, and
        // the preset's config (which must be the project's own to change).
        auto root = OrderedJson::parse(document_.bytes());
        const auto section_features = renderingSectionOf(root).value("features", OrderedJson::array());
        const auto root_instances = countGraphTemplateFeature(section_features, decomposes.feature);
        std::size_t preset_instances = 0;
        if (preset && preset->contains("config"))
            preset_instances = countGraphTemplateFeature(preset->at("config").value("features", OrderedJson::array()),
                                                         decomposes.feature);
        if (preset_instances != 0 && !current.pipeline_preset->reference.starts_with("project://"))
            return refused("project_preset_required",
                           "Fork the selected preset into the project before taking in a graph template that "
                           "decomposes one of its features",
                           {{"feature", decomposes.feature}, {"preset", current.pipeline_preset->reference}});
        if (root_instances + preset_instances == 0)
            return refused(std::string{graphTemplateFeatureAbsentCode},
                           "Graph template " + reference + " decomposes " + decomposes.feature +
                               ", which neither the rendering section nor the preset lists",
                           {{"feature", decomposes.feature}});
        if (preset_instances != 0 && !preset_digest)
            throw std::invalid_argument(
                "apply_graph_template requires base_preset_digest when the project preset lists the feature");

        GraphTemplateBody body;
        try {
            body = copyGraphTemplateBody(root, graph_template);
        } catch (const GraphRegistryError &error) {
            return refused(error.code(), error.what(), OrderedJson::parse(error.detail().dump()));
        }
        auto &section = renderingSectionOf(root);
        std::vector<std::string> added_defines;
        try {
            if (root_instances != 0) applyGraphTemplateToFeatures(section.at("features"), decomposes);
            if (preset_instances != 0) applyGraphTemplateToFeatures(preset->at("config").at("features"), decomposes);
            added_defines = addGraphTemplateShaderDefines(section, decomposes);
        } catch (const GraphTemplateError &error) {
            return refused(error.code(), error.what(), error.fields());
        }

        // The root, edited member by member so the rest stays as authored.
        auto candidate = document_.withRootValue("definitions", root.at("definitions").dump(2))
                             .withRootValue("nodes", root.at("nodes").dump(2));
        if (!Json::parse(document_.bytes()).contains("outputs"))
            candidate = candidate.withRootValue("outputs", root.at("outputs").dump(2));
        if (root_instances != 0) candidate = candidate.withSectionValue("features", section.at("features").dump(2));
        if (!added_defines.empty())
            candidate = candidate.withSectionValue("shader_defines", section.at("shader_defines").dump(2));

        auto documents = currentDocumentSet().documents();
        for (auto &document : documents)
            if (document.normalized_reference == source_normalized_reference_) document.bytes = candidate.bytes();
        if (preset_instances != 0) {
            const auto &preset_reference = current.pipeline_preset->reference;
            RenderConfigCandidateDocument written{
                .reference = preset_reference,
                .normalized_reference = dependencies_.normalize_reference(preset_reference),
                .path = dependencies_.resolve_document_path(preset_reference),
                .operation = RenderConfigDocumentOperation::replace,
                .expected = {RenderConfigDocumentExistence::present, *preset_digest},
                .bytes = preset->dump(2),
            };
            const auto existing = std::find_if(documents.begin(), documents.end(), [&](const auto &entry) {
                return entry.normalized_reference == written.normalized_reference;
            });
            if (existing != documents.end()) *existing = std::move(written);
            else documents.push_back(std::move(written));
        }
        RenderConfigCandidateDocumentSet candidate_set{source_normalized_reference_, std::move(documents)};
        // Composed as the engine will compose it, then by every variant.
        auto result = enqueueAuthoredPassTicket(method, gate, base_digest, std::move(candidate),
                                                std::move(candidate_set), std::string{},
                                                /*edits_additional_documents=*/preset_instances != 0);
        if (result.value("status", std::string{}) != "accepted") return result;
        if (preset_instances != 0) pending_.back().base_preset_digest = preset_digest;
        OrderedJson replacements = OrderedJson::object();
        if (!decomposes.remove_feature)
            for (const auto &[pass, node] : decomposes.pass_replacements) replacements[pass] = node;
        OrderedJson owners = OrderedJson::array();
        if (root_instances != 0) owners.push_back(dependencies_.source_reference);
        if (preset_instances != 0) owners.push_back(current.pipeline_preset->reference);
        result["applied"] = OrderedJson{{"template", reference},
                                        {"feature", decomposes.feature},
                                        {"feature_documents", std::move(owners)},
                                        {"removed_feature", decomposes.remove_feature},
                                        {"pass_replacements", std::move(replacements)},
                                        {"added_shader_defines", added_defines},
                                        {"definitions", body.definitions},
                                        {"nodes", body.nodes}};
        return result;
    } catch (const RenderConfigSectionUnlocated &error) {
        return rejection("render_config_section_unlocated", error.what(), {{"method", method}});
    } catch (const std::invalid_argument &) {
        throw;
    } catch (const std::exception &error) {
        return preflightRejection(error, {{"participant", "pelican.render_pipeline"}, {"method", method}});
    }
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
    auto catalog = builtinGraphModuleRegistry().catalog();
    // EL-4: render.material_pass's sockets follow its parameters, so the
    // formats each takes are listed per contract beside the modules, with
    // those of a material_outputs pass. Both are table lookups; the engine
    // shaders are not listed here (EL-4b: get_engine_shaders).
    catalog["material_pass_contracts"] = materialPassContractsJson();
    catalog["material_output_formats"] = materialOutputFormatsJson();
    return catalog;
}

OrderedJson RenderConfigEditorService::getEngineShaders(const Json &params) const {
    requireOnly(params, {}, "get_engine_shaders");
    // EL-4b: the shaders the engine ships, as a pass's stem reference loads
    // them, never inside get_graph_catalog, which Studio reads at startup.
    // EL-4c: a worker thread reflects them (once per process); until it is
    // done this answers {status: building} at once.
    return engineShadersStatusJson();
}

OrderedJson RenderConfigEditorService::getRegisteredProviders(const Json &params) const {
    requireOnly(params, {}, "get_registered_providers");
    // EL-6: the registries as they are now; a game logic reload's providers
    // show in the next call (nothing is kept between calls).
    return registeredProvidersJson();
}

OrderedJson RenderConfigEditorService::getShaderInterface(const Json &params) const {
    constexpr auto method = "get_shader_interface";
    requireOnly(params, {"reference", "refresh"}, method);
    const auto reference = requireString(params, "reference", method);
    bool refresh = false;
    if (const auto found = params.find("refresh"); found != params.end()) {
        if (!found->is_boolean()) throw std::invalid_argument("get_shader_interface field 'refresh' must be a boolean");
        refresh = found->get<bool>();
    }
    // EL-5: a worker thread reflects the stem's files; until it is done
    // this answers {status: building} at once.
    return shaderInterfaceStatusJson(reference, refresh, dependencies_.resolve_shader_reference);
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

RenderConfigEditorService::RequestedRouteDefault
RenderConfigEditorService::requestedRouteDefault(const Json &params,
                                                 std::string_view method) const {
    RequestedRouteDefault requested;
    const auto found = params.find("material_route_default");
    if (found == params.end()) return requested;
    requested.value = requestedMaterialRouteDefault(*found, method);
    if (*requested.value == MaterialRouteDefault::forward &&
        !runtime_shader_compiler_available) {
        requested.refusal = rejection(
            std::string{materialRouteDefaultUnavailableCode},
            "material_route_default \"forward\" needs the runtime shader "
            "compiler to build the forward version of the engine's built-in "
            "materials; this build has none",
            {{"value", "forward"},
             {"reason", "runtime_shader_compiler_disabled"}});
    }
    return requested;
}

bool RenderConfigEditorService::writeRouteDefault(
    AuthoredRenderConfigDocument &candidate,
    MaterialRouteDefault requested) const {
    const auto current = readMaterialRouteDefault(
        renderingSectionOrEmpty(Json::parse(document_.bytes())));
    if (requested == current) return false;
    candidate = candidate.withSectionValue(
        std::string{materialRouteDefaultKey},
        Json(std::string{materialRouteDefaultName(requested)}).dump());
    return true;
}

OrderedJson RenderConfigEditorService::queueGraphRootEdit(
    const Json &params, const std::string &base_digest,
    const std::string &operation,
    const std::function<std::optional<OrderedJson>(AuthoredRenderConfigDocument &)> &transform,
    bool source_only) {
    const auto route_default = requestedRouteDefault(params, operation);
    if (route_default.refusal) return *route_default.refusal;
    bool restart_required = false;
    auto accepted = queueRootEdit(base_digest, operation,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            if (auto refused = transform(candidate)) return refused;
            // Outside the graph edit's own catch: a root without a rendering
            // section is refused by queueRootEdit as
            // render_config_section_unlocated, as add_authored_pass refuses it.
            if (route_default.value)
                restart_required = writeRouteDefault(candidate, *route_default.value);
            return std::nullopt;
        },
        {}, std::nullopt,
        // A new default changes what every material is drawn with, so the
        // candidate is composed and judged even when the ops only move nodes.
        source_only && !route_default.value);
    if (restart_required && accepted.value("status", std::string{}) == "accepted" &&
        !pending_.empty()) {
        pending_.back().restart_required = true;
    }
    return accepted;
}

OrderedJson RenderConfigEditorService::setMaterialRouteDefault(const Json &params) {
    constexpr auto method = "set_material_route_default";
    requireOnly(params, {"base_source_digest", "material_route_default"}, method);
    const auto digest = requireString(params, "base_source_digest", method);
    if (!params.contains("material_route_default")) {
        throw std::invalid_argument(
            std::string{method} + " requires material_route_default");
    }
    const auto requested = requestedRouteDefault(params, method);
    if (requested.refusal) return *requested.refusal;
    const auto value = *requested.value;
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

namespace {

// EL-3: the rendering section keys edit_rendering_keys writes, each whole.
// Every one is read again when a generation is composed (none is read only
// at start), so none of them asks for a restart by itself.
constexpr std::array<std::string_view, 8> renderingEditableKeys{
    "draw_sort", "shader_defines", "snapshots", "graph_transforms",
    "render_strategy", "xr", "multisampling", "resolver_version"};

// EL-3: the rendering section keys another RPC writes.
struct OwnedRenderingKey {
    std::string_view key;
    std::string_view method;
};
constexpr std::array<OwnedRenderingKey, 13> renderingOwnedKeys{{
    {"features", "edit_render_features"},
    {"rendering_passes", "edit_preset_pass"},
    {"material_routing", "edit_preset_pass"},
    {"material_route_default", "set_material_route_default"},
    {"frame_node_order", "edit_frame_node_order"},
    {"scene_acceleration_structures", "edit_scene_acceleration_structures"},
    {"ray_task_acceleration_structures", "edit_scene_acceleration_structures"},
    {"ray_task_callable_records", "edit_ray_callable_records"},
    {"pipeline", "fork_render_pipeline"},
    {"render_targets", "edit_render_targets"},
    // EL-2.
    {"target_planning", "edit_target_planning"},
    {"vulkan_plan_pins", "edit_vulkan_plan_pins"},
    {"vulkan_physical_fragments", "put_vulkan_physical_fragment"},
}};

} // namespace

std::optional<OrderedJson> RenderConfigEditorService::renderingKeyRefusal(
    const std::string &key, bool selects_preset) const {
    for (const auto &owned : renderingOwnedKeys) {
        if (owned.key != key) continue;
        return rejection(
            "rendering_key_owned",
            "rendering." + key + " is written by " + std::string{owned.method} +
                "; edit_rendering_keys does not write it",
            {{"key", key}, {"method", std::string{owned.method}}});
    }
    if (std::ranges::find(renderingEditableKeys, key) ==
        renderingEditableKeys.end()) {
        std::string listed;
        for (const auto editable : renderingEditableKeys) {
            if (!listed.empty()) listed += ", ";
            listed += editable;
        }
        return rejection(
            "rendering_key_not_editable",
            "rendering." + key + " is not a key edit_rendering_keys writes; it writes " +
                listed,
            {{"key", key}});
    }
    if (!selects_preset) return std::nullopt;
    // What resolveRenderPipelinePreset lets a root that selects a preset say.
    if (key == "multisampling") {
        return rejection(
            "rendering_key_not_allowed_here",
            "a rendering section that selects a pipeline preset takes "
            "multisampling only as pipeline.settings.msaa (the root is written "
            "whole by restore_render_documents); a direct root takes "
            "rendering.multisampling",
            {{"key", key}, {"reason", "preset_root_takes_pipeline_settings_msaa"}});
    }
    if (key == "resolver_version") {
        return rejection(
            "rendering_key_not_allowed_here",
            "a rendering section that selects a pipeline preset does not take "
            "resolver_version; it belongs in the preset's config (a project "
            "preset after fork_render_pipeline) or in a direct root's "
            "rendering section",
            {{"key", key}, {"reason", "not_in_preset_root_keys"}});
    }
    return std::nullopt;
}

OrderedJson RenderConfigEditorService::editRenderingKeys(const Json &params) {
    constexpr std::string_view method = "edit_rendering_keys";
    requireOnly(params, {"base_source_digest", "set", "remove"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    if (!isLowerHexDigest(base_digest))
        throw std::invalid_argument(
            "edit_rendering_keys base_source_digest must be 64 lowercase hex characters");
    const auto set = params.find("set");
    if (set != params.end() && !set->is_object())
        throw std::invalid_argument("edit_rendering_keys set must be an object of key: value");
    std::vector<std::string> keys;
    if (set != params.end())
        for (const auto &[key, value] : set->items()) {
            (void)value;
            keys.push_back(key);
        }
    std::vector<std::string> removed;
    if (const auto remove = params.find("remove"); remove != params.end()) {
        if (!remove->is_array())
            throw std::invalid_argument("edit_rendering_keys remove must be an array of keys");
        for (const auto &entry : *remove) {
            if (!entry.is_string())
                throw std::invalid_argument("edit_rendering_keys remove must be an array of keys");
            const auto key = entry.get<std::string>();
            if (set != params.end() && set->contains(key))
                throw std::invalid_argument("edit_rendering_keys cannot both set and remove " + key);
            removed.push_back(key);
            keys.push_back(key);
        }
    }
    if (keys.empty())
        throw std::invalid_argument("edit_rendering_keys requires a key in set or remove");
    if (!dependencies_.resolve_authoring_context)
        return rejection("method_unavailable",
                         "edit_rendering_keys requires the render authoring resolver",
                         {{"method", std::string{method}}});

    // Every key is judged before anything is written: one refusal and the
    // ticket writes none of them.
    const bool selects_preset =
        renderingSectionOrEmpty(Json::parse(document_.bytes())).contains("pipeline");
    for (const auto &key : keys)
        if (auto refused = renderingKeyRefusal(key, selects_preset)) return *refused;

    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(
            gate, base_digest, method,
            "render config editing is currently disabled")) {
        return *rejected;
    }
    auto candidate = document_;
    try {
        if (set != params.end())
            for (const auto &[key, value] : set->items())
                candidate = candidate.withSectionValue(key, value.dump(2));
        for (const auto &key : removed) candidate = candidate.withoutSectionValue(key);
    } catch (const RenderConfigSectionUnlocated &error) {
        return rejection("render_config_section_unlocated", error.what(),
                         {{"method", std::string{method}}});
    }
    auto documents = currentDocumentSet().documents();
    const auto root_document = std::find_if(
        documents.begin(), documents.end(), [&](const auto &entry) {
            return entry.normalized_reference == source_normalized_reference_;
        });
    if (root_document == documents.end())
        throw std::logic_error("edit_rendering_keys candidate lost its root document");
    root_document->bytes = candidate.bytes();
    // The values are judged by the candidate as a whole: the same composition
    // (and graph variants) every authored ticket goes through, then the
    // runtime's recomposition when it commits.
    return enqueueAuthoredPassTicket(
        std::string{method}, gate, base_digest, std::move(candidate),
        RenderConfigCandidateDocumentSet{source_normalized_reference_, std::move(documents)},
        std::string{});
}

// --- EL-2: the planner's decisions a rendering root declares.

namespace {

constexpr std::array<std::string_view, 3> planFamilies{"flat", "preview", "xr"};

std::string requirePlanFamily(const Json &params, std::string_view method) {
    auto family = requireString(params, "family", method);
    if (std::ranges::find(planFamilies, family) == planFamilies.end())
        throw std::invalid_argument(std::string{method} +
                                    " family must be \"flat\", \"preview\" or \"xr\"");
    return family;
}

// The names target_planning.graphs.<graph> may name, as the planner reads the
// composed config: the frame graph's nodes, and the logical resources its
// shadow graph declares (declared and history targets, what a node writes,
// swapchain when a node reads it). nullopt: the composition has no such graph.
struct PlanningNames {
    std::set<std::string, std::less<>> nodes;
    std::set<std::string, std::less<>> resources;
};

std::optional<PlanningNames> planningNamesOf(const Json &composed, std::string_view graph) {
    for (const auto &definition : parseFrameGraphDefinitionsFromConfigJson(composed)) {
        if (definition.name != graph) continue;
        PlanningNames names;
        names.resources.insert(definition.declared_resources.begin(),
                               definition.declared_resources.end());
        names.resources.insert(definition.history_resources.begin(),
                               definition.history_resources.end());
        for (const auto &node : definition.nodes) {
            names.nodes.insert(node.name);
            names.resources.insert(node.writes.begin(), node.writes.end());
            if (std::ranges::find(node.reads, std::string{"swapchain"}) != node.reads.end())
                names.resources.insert("swapchain");
        }
        return names;
    }
    return std::nullopt;
}

// {name: object | null} applied to `entries`: null erases the name; an
// object's fields replace the entry's, a null field erasing that field. An
// entry left with no field is erased.
void mergePlanningEntries(OrderedJson &entries, const Json &edits) {
    if (!entries.is_object()) entries = OrderedJson::object();
    for (const auto &[name, edit] : edits.items()) {
        if (edit.is_null()) {
            entries.erase(name);
            continue;
        }
        auto &entry = entries[name];
        if (!entry.is_object()) entry = OrderedJson::object();
        for (const auto &[field, value] : edit.items()) {
            if (value.is_null()) entry.erase(field);
            else entry[field] = value;
        }
        if (entry.empty()) entries.erase(name);
    }
}

void eraseIfEmpty(OrderedJson &holder, const std::string &key) {
    const auto found = holder.find(key);
    if (found != holder.end() && (found->is_object() || found->is_array()) &&
        found->empty())
        holder.erase(found);
}

// The package a planning RPC writes (null: erase it), judged by the parser the
// compiler reads it with, and the graph a null erases (empty: every graph).
struct PlanPackageRequest {
    Json package;
    std::string graph;
};

PlanPackageRequest requirePlanPackage(const Json &params, const std::string &field,
                                      std::string_view method,
                                      const std::function<void(const Json &)> &parse) {
    const auto found = params.find(field);
    if (found == params.end())
        throw std::invalid_argument(std::string{method} + " requires " + field +
                                    " (a package, or null)");
    PlanPackageRequest request{.package = *found, .graph = {}};
    if (const auto graph = params.find("graph"); graph != params.end()) {
        if (!found->is_null())
            throw std::invalid_argument(std::string{method} + " takes graph only with a null " +
                                        field + "; a package names its own graph");
        if (!graph->is_string() || graph->get_ref<const std::string &>().empty())
            throw std::invalid_argument(std::string{method} + " graph must be a non-empty string");
        request.graph = graph->get<std::string>();
    }
    if (found->is_null()) return request;
    try {
        parse(*found);
    } catch (const std::exception &error) {
        throw std::invalid_argument(std::string{method} + " " + field + ": " + error.what());
    }
    return request;
}

// The family's packages with the request's package in place of the one for
// its graph (appended when the family has none for it); a null package erases
// the one for the request's graph, or every one of the family. A family left
// with none is erased, and the key with it when no family is left.
std::optional<OrderedJson> withFamilyPackage(std::optional<OrderedJson> current,
                                             const std::string &family,
                                             const PlanPackageRequest &request) {
    auto value = current && current->is_object() ? std::move(*current) : OrderedJson::object();
    const auto target =
        request.package.is_null() ? request.graph : request.package.value("graph", std::string{});
    OrderedJson kept = OrderedJson::array();
    bool placed = false;
    if (const auto packages = value.find(family);
        packages != value.end() && packages->is_array()) {
        for (auto &entry : *packages) {
            const bool same = target.empty() ||
                              (entry.is_object() && entry.value("graph", std::string{}) == target);
            if (!same) {
                kept.push_back(std::move(entry));
            } else if (!request.package.is_null() && !placed) {
                kept.push_back(request.package);
                placed = true;
            }
        }
    }
    if (!request.package.is_null() && !placed) kept.push_back(request.package);
    if (kept.empty()) value.erase(family);
    else value[family] = std::move(kept);
    if (value.empty()) return std::nullopt;
    return value;
}

} // namespace

OrderedJson RenderConfigEditorService::queuePlanningKeyEdit(
    std::string_view method, const std::string &base_digest, std::string_view key,
    const std::function<PlanningKeyValue(std::optional<OrderedJson>,
                                         const ResolvedRenderConfigAuthoringContext &)> &transform) {
    const std::string method_name{method};
    const std::string key_name{key};
    if (!isLowerHexDigest(base_digest))
        throw std::invalid_argument(method_name +
                                    " base_source_digest must be 64 lowercase hex characters");
    if (!dependencies_.resolve_authoring_context)
        return rejection("method_unavailable",
                         method_name + " requires the render authoring resolver",
                         {{"method", method_name}});
    const auto gate = gateSnapshot();
    if (auto rejected = checkAuthoringPreconditions(
            gate, base_digest, method, "render config editing is currently disabled")) {
        return *rejected;
    }
    std::optional<OrderedJson> current;
    try {
        const auto root = OrderedJson::parse(document_.bytes());
        const auto &section = renderingSectionOf(root);
        if (const auto found = section.find(key_name); found != section.end()) current = *found;
    } catch (const RenderConfigSectionUnlocated &error) {
        return rejection("render_config_section_unlocated", error.what(), {{"method", method_name}});
    }
    const OrderedJson preflight_details{{"participant", "pelican.render_pipeline"},
                                        {"method", method_name}};
    PlanningKeyValue edited;
    try {
        const auto context = dependencies_.resolve_authoring_context(currentDocumentSet());
        // What resolveRenderPipelinePreset refuses: a root that selects a
        // preset does not override the preset's own declaration of the key.
        if (context.pipeline_preset_bytes) {
            const auto preset = Json::parse(*context.pipeline_preset_bytes);
            const auto config = preset.find("config");
            if (config != preset.end() && config->is_object() && config->contains(key_name)) {
                return rejection(
                    "rendering_key_not_allowed_here",
                    "the pipeline preset this rendering section selects declares " + key_name +
                        ", which a section that selects a preset cannot override; fork the "
                        "preset (fork_render_pipeline) and edit it there",
                    {{"key", key_name}, {"reason", "preset_declares_key"}});
            }
        }
        edited = transform(std::move(current), context);
    } catch (const std::invalid_argument &) {
        throw;
    } catch (const std::exception &error) {
        return preflightRejection(error, preflight_details);
    }
    if (edited.refusal) return *edited.refusal;
    auto candidate = document_;
    try {
        candidate = edited.value ? candidate.withSectionValue(key_name, edited.value->dump(2))
                                 : candidate.withoutSectionValue(key_name);
    } catch (const RenderConfigSectionUnlocated &error) {
        return rejection("render_config_section_unlocated", error.what(), {{"method", method_name}});
    }
    auto documents = currentDocumentSet().documents();
    const auto root_document = std::find_if(
        documents.begin(), documents.end(), [&](const auto &entry) {
            return entry.normalized_reference == source_normalized_reference_;
        });
    if (root_document == documents.end())
        throw std::logic_error(method_name + " candidate lost its root document");
    root_document->bytes = candidate.bytes();
    // The values are judged by the candidate as a whole: its composition (and
    // graph variants) here, then the runtime's recomposition, which compiles
    // the target plans, when it commits.
    return enqueueAuthoredPassTicket(
        method_name, gate, base_digest, std::move(candidate),
        RenderConfigCandidateDocumentSet{source_normalized_reference_, std::move(documents)},
        std::string{});
}

std::optional<OrderedJson> RenderConfigEditorService::physicalFragmentStaleRefusal(
    const std::exception_ptr &error, std::string_view root_bytes, OrderedJson details) const {
    if (!error) return std::nullopt;
    try {
        std::rethrow_exception(error);
    } catch (const VulkanPhysicalFragmentStale &stale) {
        const auto &info = stale.info();
        // The family the root's section declares the package under. A
        // package a preset declares has none here.
        try {
            const auto root = Json::parse(root_bytes);
            const auto &section = renderingSectionOrEmpty(root);
            if (const auto fragments = section.find("vulkan_physical_fragments");
                fragments != section.end() && fragments->is_object()) {
                for (const auto &[family, packages] : fragments->items()) {
                    if (!packages.is_array()) continue;
                    for (const auto &package : packages)
                        if (package.is_object() &&
                            package.value("graph", std::string{}) == info.graph)
                            details["family"] = family;
                }
            }
        } catch (const std::exception &) {
        }
        details["graph"] = info.graph;
        details["fingerprint"] = info.fingerprint;
        details["expected"] = info.expected;
        details["actual"] = info.actual;
        return rejection("physical_fragment_stale", stale.what(), std::move(details));
    } catch (...) {
    }
    return std::nullopt;
}

OrderedJson RenderConfigEditorService::editTargetPlanning(const Json &params) {
    constexpr std::string_view method = "edit_target_planning";
    requireOnly(params, {"base_source_digest", "graph", "nodes", "resources"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto graph = requireString(params, "graph", method);
    if (graph.empty())
        throw std::invalid_argument("edit_target_planning graph must not be empty");
    const auto entries = [&](const char *field) -> const Json * {
        const auto found = params.find(field);
        if (found == params.end()) return nullptr;
        const auto shape = std::string{"edit_target_planning "} + field +
                           " must be an object of name: object | null";
        if (!found->is_object()) throw std::invalid_argument(shape);
        for (const auto &[name, edit] : found->items())
            if (name.empty() || !(edit.is_object() || edit.is_null()))
                throw std::invalid_argument(shape);
        return &*found;
    };
    const auto *nodes = entries("nodes");
    const auto *resources = entries("resources");
    if ((nodes == nullptr || nodes->empty()) && (resources == nullptr || resources->empty()))
        throw std::invalid_argument("edit_target_planning requires a name in nodes or resources");

    return queuePlanningKeyEdit(
        method, base_digest, "target_planning",
        [&](std::optional<OrderedJson> current,
            const ResolvedRenderConfigAuthoringContext &context) -> PlanningKeyValue {
            const auto unknown = [&](std::string_view kind, const std::string &name) {
                return rejection(
                    "target_planning_name_unknown",
                    "target_planning." + graph + ": the current composition has no " +
                        std::string{kind} + " named " + name,
                    {{"graph", graph}, {"kind", std::string{kind}}, {"name", name}});
            };
            // The names are the expanded ones get_frame_plan shows. FG-1 maps a
            // fold group's local names onto them before this point.
            const auto names = planningNamesOf(context.config, graph);
            if (!names) return {.value = std::nullopt, .refusal = unknown("graph", graph)};
            auto planning =
                current && current->is_object() ? std::move(*current) : OrderedJson::object();
            if (!planning["graphs"].is_object()) planning["graphs"] = OrderedJson::object();
            auto &declared = planning["graphs"][graph];
            if (!declared.is_object()) declared = OrderedJson::object();
            for (const auto &[field, kind, edits, known] :
                 {std::tuple{"nodes", "node", nodes, &names->nodes},
                  std::tuple{"resources", "resource", resources, &names->resources}}) {
                if (edits == nullptr) continue;
                const auto authored = declared.find(field);
                for (const auto &[name, edit] : edits->items()) {
                    if (known->contains(name)) continue;
                    // A name the composition no longer has may still be erased.
                    if (edit.is_null() && authored != declared.end() && authored->is_object() &&
                        authored->contains(name))
                        continue;
                    return {.value = std::nullopt, .refusal = unknown(kind, name)};
                }
                // FG-1: an explicit false against what a verified fold group
                // decides for the same name.
                for (const auto &[name, edit] : edits->items()) {
                    if (!edit.is_object()) continue;
                    for (const auto &[decision, value] : edit.items()) {
                        if (!value.is_boolean()) continue;
                        if (const auto conflict = graphFoldDecisionConflict(
                                context.fold_planning, graph, kind, name, decision, value.get<bool>()))
                            return {.value = std::nullopt,
                                    .refusal = rejection(
                                        std::string{graphFoldDecisionConflictCode},
                                        "target_planning." + graph + ": fold group " + conflict->group +
                                            " decides " + decision + " for " + name,
                                        OrderedJson::parse(
                                            graphFoldDecisionConflictDetail(*conflict).dump()))};
                    }
                }
                mergePlanningEntries(declared[field], *edits);
                eraseIfEmpty(declared, field);
            }
            eraseIfEmpty(planning["graphs"], graph);
            eraseIfEmpty(planning, "graphs");
            if (planning.empty()) return {};
            return {.value = std::move(planning), .refusal = std::nullopt};
        });
}

OrderedJson RenderConfigEditorService::editVulkanPlanPins(const Json &params) {
    constexpr std::string_view method = "edit_vulkan_plan_pins";
    requireOnly(params, {"base_source_digest", "family", "pin", "graph"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto family = requirePlanFamily(params, method);
    const auto request = requirePlanPackage(params, "pin", method, [](const Json &document) {
        (void)vulkanTargetPlanPinPackageFromJson(document);
    });
    return queuePlanningKeyEdit(
        method, base_digest, "vulkan_plan_pins",
        [&](std::optional<OrderedJson> current,
            const ResolvedRenderConfigAuthoringContext &) -> PlanningKeyValue {
            return {.value = withFamilyPackage(std::move(current), family, request),
                    .refusal = std::nullopt};
        });
}

OrderedJson RenderConfigEditorService::putVulkanPhysicalFragment(const Json &params) {
    constexpr std::string_view method = "put_vulkan_physical_fragment";
    requireOnly(params, {"base_source_digest", "family", "fragment", "graph"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto family = requirePlanFamily(params, method);
    const auto request = requirePlanPackage(params, "fragment", method, [](const Json &document) {
        (void)vulkanPhysicalFragmentPackageFromJson(document);
    });
    // Whether the fragment's fingerprints are the plan's is the runtime
    // linker's to say (physical_fragment_stale when they are not).
    return queuePlanningKeyEdit(
        method, base_digest, "vulkan_physical_fragments",
        [&](std::optional<OrderedJson> current,
            const ResolvedRenderConfigAuthoringContext &) -> PlanningKeyValue {
            return {.value = withFamilyPackage(std::move(current), family, request),
                    .refusal = std::nullopt};
        });
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
        // An editable task is declared in a project feature or a project
        // preset, never in the rendering root (MG-2b: the catalog lists the
        // root's tasks read-only, so the check above already refused them).
        // FR-4: read as the editor holds it, so inside edit_batch the edit
        // builds on the batch's earlier edits; the caller's digest is the
        // document's before the batch.
        const auto reference = selected->at("source_reference").get<std::string>();
        const auto path = dependencies_.resolve_document_path(reference);
        const auto held = heldDocument(reference);
        if (held.base_digest != task_digest)
            return rejection("external_modification", "The task source changed since it was read");
        auto source = OrderedJson::parse(held.bytes);
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
        requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "output", "shader", "ray_query", "view_select"}, method);
        bool changed = params.contains("view_select");
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
        requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "draw", "raster_state", "ray_query", "view_select"}, method);
        bool changed = params.contains("view_select");
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
        requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "ray_query", "material_settings", "material_routing", "view_select"}, method);
        if (!params.contains("input") && !params.contains("output") && !params.contains("ray_query") &&
            !params.contains("material_settings") && !params.contains("material_routing") &&
            !params.contains("view_select"))
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
        requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "ray_query", "uses_light_data", "push_constants", "clear_colors", "view_select"}, method);
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
    // LP-Va2: the views of its family the pass runs on, for every pass type.
    // Omitted keeps the value, null removes the member, and a value is read by
    // the rule the compiler reads it with (parseRenderViewSelect), so a
    // malformed one is refused here for the same reason. Whether the axis
    // exists is the recompile's check (graph.view_select_axis_unknown@1).
    if (params.contains("view_select")) {
        const auto &value = params.at("view_select");
        if (value.is_null()) {
            pass.erase("view_select");
        } else {
            (void)parseRenderViewSelect(Json{{"view_select", value}},
                std::string{method} + " pass '" + pass.at("name").get<std::string>() + "'");
            pass["view_select"] = value;
        }
    }
}

} // namespace

OrderedJson RenderConfigEditorService::editPresetPass(const Json &params) {
    constexpr auto method = "edit_preset_pass";
    requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "draw", "raster_state", "ray_query", "material_settings", "material_routing", "uses_light_data", "push_constants", "clear_colors", "view_select"}, method);
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
        } else {
            reference = current.pipeline_preset->reference;
        }
        // FR-4: the preset or feature as the editor holds it, so inside
        // edit_batch the edit builds on the batch's earlier edits; the
        // caller's digest is the document's before the batch.
        const auto held = heldDocument(reference);
        if (held.base_digest != preset_digest)
            return rejection("external_modification", "The selected pass source changed since it was read");
        auto preset = OrderedJson::parse(held.bytes);
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
    // RD-1: restore_render_documents names no fragment; its refusals and its
    // accepted answer carry the method instead.
    const auto preflight_details = [&] {
        OrderedJson details{{"participant", "pelican.render_pipeline"}};
        if (reference.empty()) details["method"] = operation;
        else details["fragment_reference"] = reference;
        return details;
    };
    if (batch_) {
        // FR-3: edit_batch composes its last candidate once, with this check.
        batch_->composes = true;
        if (check) batch_->checks.push_back(check);
    } else {
        try {
            // This is the decisive staged-file contrast: neither the feature nor
            // preset resolver may bypass the request-local document set.
            const auto candidate =
                dependencies_.resolve_authoring_context(candidate_documents);
            if (check) check(candidate);
        } catch (const std::exception &error) {
            return preflightRejection(error, preflight_details());
        }
        // G-6: the same candidate, by every variant the engine will compose it
        // with when it publishes.
        if (auto refused = preflightGraphVariants(
                candidate_documents, preflight_details())) {
            return *refused;
        }
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
    OrderedJson accepted{{"ticket", ticket_id},
                         {"status", "accepted"},
                         {"base_source_digest", digestJson(base_digest)}};
    if (!reference.empty()) accepted["fragment_reference"] = reference;
    return accepted;
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
    const auto requested_route_default =
        requestedRouteDefault(params, "add_authored_pass");
    if (requested_route_default.refusal) return *requested_route_default.refusal;

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
    if (requested_route_default.value) {
        try {
            restart_required =
                writeRouteDefault(root, *requested_route_default.value);
        } catch (const RenderConfigSectionUnlocated &error) {
            return rejection("render_config_section_unlocated",
                             error.what(),
                             {{"method", "add_authored_pass"}});
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
    // FR-3: edit_batch compared every fragment before its first edit.
    if (managed != managed_documents_.end() && !batch_) {
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
        // FR-3: edit_batch compared every fragment before its first edit.
        if (!batch_) try {
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

namespace {

// RD-1: the documents a committed candidate wrote, {reference: sha256 hex}
// (null for one it erased), keyed by the reference the candidate names it by.
// A document the candidate carries unchanged is not listed. Studio uses the
// digest as the next base_digest of restore_render_documents.
OrderedJson writtenDocumentDigests(
    const RenderConfigCandidateDocumentSet &documents) {
    OrderedJson written = OrderedJson::object();
    for (const auto &document : documents.documents()) {
        if (document.operation == RenderConfigDocumentOperation::erase) {
            written[document.reference] = nullptr;
            continue;
        }
        const auto digest = renderConfigSourceDigest(document.bytes);
        if (document.expected.existence ==
                RenderConfigDocumentExistence::missing ||
            digest != document.expected.digest)
            written[document.reference] = digest;
    }
    return written;
}

std::string_view renderDocumentSchema(std::string_view kind) {
    return kind == "preset" ? "pelican.render_pipeline"
                            : "pelican.render_feature";
}

// RD-1: whether bytes are a render document of the kind the composition
// reads them as. A root is judged by the editor's own inspection instead.
bool isRenderDocumentOfKind(std::string_view bytes, std::string_view kind) {
    try {
        const auto document = Json::parse(bytes);
        return document.is_object() &&
               document.value("schema", std::string{}) ==
                   renderDocumentSchema(kind);
    } catch (const Json::exception &) {
        return false;
    }
}

void appendFeatureReferences(const Json &features,
                             std::vector<std::string> &references) {
    if (!features.is_array()) return;
    for (const auto &entry : features) {
        if (entry.is_string()) {
            references.push_back(entry.get<std::string>());
        } else if (entry.is_object()) {
            const auto ref = entry.find("ref");
            if (ref != entry.end() && ref->is_string())
                references.push_back(ref->get<std::string>());
        }
    }
}

} // namespace

std::vector<RenderConfigEditorService::WritableRenderDocument>
RenderConfigEditorService::writableRenderDocuments() const {
    return renderDocumentsOf(currentDocumentSet(), document_.bytes());
}

std::vector<RenderConfigEditorService::WritableRenderDocument>
RenderConfigEditorService::renderDocumentsOf(
    const RenderConfigCandidateDocumentSet &candidate,
    std::string_view root_bytes) const {
    std::vector<WritableRenderDocument> documents;
    documents.push_back(WritableRenderDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .kind = "root",
    });
    if (!dependencies_.resolve_authoring_context ||
        !dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path)
        return documents;
    std::optional<ResolvedRenderConfigAuthoringContext> composed;
    try {
        composed.emplace(dependencies_.resolve_authoring_context(candidate));
    } catch (const std::exception &) {
        // BS-1: a root that does not compose (the one an engine on its
        // fallback generation was refused) still lists itself and the
        // documents its rendering section names that are on disk, so a client
        // can read and fix them. Where it composes, the preflight that
        // follows says why it does not.
        const auto preset_on_disk =
            [&](const std::string &reference) -> std::optional<std::string> {
            try {
                const auto path = dependencies_.resolve_document_path(reference);
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error) && !error)
                    return readFileBytes(path);
            } catch (const std::exception &) {
            }
            return std::nullopt;
        };
        for (auto &entry : namedRenderDocuments(root_bytes, preset_on_disk)) {
            try {
                std::error_code error;
                if (!std::filesystem::is_regular_file(entry.path, error) || error)
                    continue;
                // BS-1b: one that is not JSON is listed too, to be fixed.
                const auto bytes = readFileBytes(entry.path);
                if (Json::accept(bytes) &&
                    !isRenderDocumentOfKind(bytes, entry.kind))
                    continue;
            } catch (const std::exception &) {
                continue;
            }
            documents.push_back(std::move(entry));
        }
        return documents;
    }
    const auto &current = *composed;
    // A document is writable only when all of these hold: it is not an
    // engine document or a fragment address, it resolves to a regular file
    // inside the project root (symbolic links resolved), and its bytes are the
    // kind of render document the composition reads it as. A scene, code or
    // asset document fails the last one even when a list names it.
    const auto consider = [&](const std::string &reference,
                              std::string_view kind) {
        if (reference.empty() || reference.starts_with("engine://") ||
            reference.find('#') != std::string::npos)
            return;
        try {
            auto normalized = dependencies_.normalize_reference(reference);
            if (std::any_of(documents.begin(), documents.end(),
                            [&](const auto &entry) {
                                return entry.normalized_reference == normalized;
                            }))
                return;
            auto path = dependencies_.resolve_document_path(reference);
            if (!isWithinDirectory(dependencies_.project_root, path)) return;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error) return;
            if (!isRenderDocumentOfKind(readFileBytes(path), kind)) return;
            documents.push_back(WritableRenderDocument{
                .reference = reference,
                .normalized_reference = std::move(normalized),
                .path = std::move(path),
                .kind = std::string{kind},
            });
        } catch (const std::exception &) {
            // A reference that does not resolve to a project file is not one
            // this service writes.
        }
    };
    // The forked preset: only a project:// one is the project's to write, as
    // every preset edit requires.
    std::vector<std::string> features;
    if (current.pipeline_preset &&
        current.pipeline_preset->reference.starts_with("project://")) {
        consider(current.pipeline_preset->reference, "preset");
    }
    if (current.pipeline_preset_bytes) {
        try {
            const auto preset = Json::parse(*current.pipeline_preset_bytes);
            if (preset.is_object() && preset.contains("config") &&
                preset.at("config").is_object())
                appendFeatureReferences(
                    preset.at("config").value("features", Json::array()),
                    features);
        } catch (const Json::exception &) {
        }
    }
    appendFeatureReferences(
        renderingSectionOrEmpty(Json::parse(root_bytes))
            .value("features", Json::array()),
        features);
    appendFeatureReferences(current.config.value("features", Json::array()),
                            features);
    for (const auto &pass : current.pass_provenance)
        features.push_back(pass.provider_reference);
    for (const auto &resource : current.resource_provenance)
        features.push_back(resource.provider_reference);
    for (const auto &reference : features) consider(reference, "feature");
    return documents;
}

const RenderConfigEditorService::WritableRenderDocument *
RenderConfigEditorService::findWritableRenderDocument(
    const std::vector<WritableRenderDocument> &writable,
    const std::string &reference) const {
    // Refused before normalization: a normalizer that drops a fragment
    // address or aliases an engine name must not turn it into a document.
    if (reference.empty() || reference.starts_with("engine://") ||
        reference.find('#') != std::string::npos)
        return nullptr;
    std::string normalized;
    try {
        normalized = dependencies_.normalize_reference(reference);
    } catch (const std::exception &) {
        return nullptr;
    }
    const auto found = std::find_if(
        writable.begin(), writable.end(), [&](const auto &entry) {
            return entry.normalized_reference == normalized;
        });
    return found == writable.end() ? nullptr : &*found;
}

std::vector<RenderConfigEditorService::WritableRenderDocument>
RenderConfigEditorService::namedRenderDocuments(
    std::string_view root_bytes,
    const std::function<std::optional<std::string>(const std::string &)>
        &preset_bytes) const {
    std::vector<WritableRenderDocument> named;
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path)
        return named;
    std::vector<std::pair<std::string, std::string_view>> references;
    try {
        const auto root = Json::parse(root_bytes);
        const auto &section = renderingSectionOrEmpty(root);
        std::vector<std::string> features;
        appendFeatureReferences(section.value("features", Json::array()),
                                features);
        const auto pipeline = section.find("pipeline");
        if (pipeline != section.end() && pipeline->is_object()) {
            const auto preset = pipeline->find("preset");
            if (preset != pipeline->end() && preset->is_string()) {
                const auto reference = preset->get<std::string>();
                // Only a project:// preset is the project's, as every preset
                // edit requires.
                if (reference.starts_with("project://"))
                    references.emplace_back(reference, "preset");
                try {
                    if (const auto bytes = preset_bytes(reference)) {
                        const auto document = Json::parse(*bytes);
                        if (document.is_object() &&
                            document.contains("config") &&
                            document.at("config").is_object())
                            appendFeatureReferences(
                                document.at("config").value("features",
                                                            Json::array()),
                                features);
                    }
                } catch (const std::exception &) {
                    // A preset that does not read names no features; the
                    // candidate's preflight says why it does not compose.
                }
            }
        }
        for (auto &feature : features)
            references.emplace_back(std::move(feature), "feature");
    } catch (const std::exception &) {
        // A root that does not parse names nothing; its content is refused
        // by the restore's own content check.
    }
    for (const auto &[reference, kind] : references) {
        if (reference.empty() || reference.starts_with("engine://") ||
            reference.find('#') != std::string::npos)
            continue;
        try {
            auto normalized = dependencies_.normalize_reference(reference);
            if (normalized == source_normalized_reference_ ||
                std::any_of(named.begin(), named.end(), [&](const auto &entry) {
                    return entry.normalized_reference == normalized;
                }))
                continue;
            auto path = dependencies_.resolve_document_path(reference);
            // The path, or for a document to create the directory it lands
            // in, resolves inside the project (symbolic links resolved).
            if (!isWithinDirectory(dependencies_.project_root, path)) continue;
            named.push_back(WritableRenderDocument{
                .reference = reference,
                .normalized_reference = std::move(normalized),
                .path = std::move(path),
                .kind = std::string{kind},
            });
        } catch (const std::exception &) {
        }
    }
    return named;
}

bool RenderConfigEditorService::isAbsentProjectDocument(
    const std::string &reference) const {
    if (reference.empty() || reference.starts_with("engine://") ||
        reference.find('#') != std::string::npos ||
        !dependencies_.resolve_document_path)
        return false;
    try {
        const auto path = dependencies_.resolve_document_path(reference);
        if (!isWithinDirectory(dependencies_.project_root, path)) return false;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        // A not-found path may also set `error`; the type says it.
        return status.type() == std::filesystem::file_type::not_found;
    } catch (const std::exception &) {
        return false;
    }
}

namespace {

std::string asciiLower(std::string text) {
    for (auto &character : text)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return text;
}

// Whether `relative` lies under `directory` (both project-relative, compared
// component by component and without case: a Windows project is).
bool isUnderRelativeDirectory(const std::filesystem::path &relative,
                              const std::filesystem::path &directory) {
    auto relative_it = relative.begin();
    for (const auto &component : directory) {
        if (relative_it == relative.end() ||
            asciiLower(relative_it->generic_string()) !=
                asciiLower(component.generic_string()))
            return false;
        ++relative_it;
    }
    return relative_it != relative.end();
}

} // namespace

std::optional<std::string> RenderConfigEditorService::reservedProjectLocation(
    const std::filesystem::path &path) const {
    std::error_code error;
    const auto root =
        std::filesystem::weakly_canonical(dependencies_.project_root, error);
    if (error) return std::nullopt;
    const auto candidate = std::filesystem::weakly_canonical(path, error);
    if (error) return std::nullopt;
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.begin() == relative.end()) return std::nullopt;

    // The project template's own places (src/project/projecttemplate.cpp and
    // the .gitignore it writes): code, scenes, assets, input, UI, vendored
    // packages (install_package), editor metadata and build output.
    const auto first = asciiLower(relative.begin()->generic_string());
    for (const std::string_view reserved :
         {".pelican", "code", "assets", "scenes", "input", "ui", "packages",
          "build", "out", "output"}) {
        if (first == reserved && std::next(relative.begin()) != relative.end())
            return std::string{reserved} + "/";
    }
    if (first.starts_with("build-") &&
        std::next(relative.begin()) != relative.end())
        return relative.begin()->generic_string() + "/";

    // The directories project.json's basic_config paths name (a project may
    // keep its scene or UI elsewhere), except one that holds the rendering
    // config root.
    std::error_code root_error;
    const auto root_relative =
        std::filesystem::weakly_canonical(dependencies_.source_path, root_error)
            .lexically_relative(root);
    std::optional<std::string> found;
    const auto consider = [&](std::string value) {
        if (found) return;
        if (value.starts_with("project://")) value.erase(0, 10);
        // Only document paths; a window title is not a place.
        if (!value.ends_with(".json") || value.find("://") != std::string::npos)
            return;
        const auto parent =
            std::filesystem::path{value}.lexically_normal().parent_path();
        if (parent.empty() || parent.is_absolute() ||
            parent.begin()->generic_string() == "..")
            return;
        if (root_error || isUnderRelativeDirectory(root_relative, parent))
            return;
        if (isUnderRelativeDirectory(relative, parent))
            found = parent.generic_string() + "/";
    };
    try {
        const auto project =
            Json::parse(readFileBytes(dependencies_.project_root / "project.json"));
        const auto basic = project.find("basic_config");
        if (basic == project.end() || !basic->is_object()) return std::nullopt;
        std::function<void(const Json &)> visit = [&](const Json &value) {
            if (value.is_string()) consider(value.get<std::string>());
            else if (value.is_object() || value.is_array())
                for (const auto &child : value) visit(child);
        };
        for (const auto &[key, value] : basic->items())
            if (key != "rendering_config_json") visit(value);
    } catch (const std::exception &) {
    }
    return found;
}

OrderedJson RenderConfigEditorService::renderDocumentNotWritable(
    std::string_view method, const std::string &reference) const {
    std::string reason = "not_render_document";
    std::string explanation =
        "it is not a render document the current composition writes (the "
        "root, the forked preset or a project feature document) or one the "
        "restored root or preset names";
    if (reference.starts_with("engine://")) {
        reason = "engine_document";
        explanation = "engine documents are never written";
    } else if (reference.find('#') != std::string::npos) {
        reason = "fragment_address";
        explanation = "a fragment address is not a document";
    } else {
        bool inside = false;
        try {
            if (dependencies_.resolve_document_path)
                inside = isWithinDirectory(
                    dependencies_.project_root,
                    dependencies_.resolve_document_path(reference));
        } catch (const std::exception &) {
        }
        if (!inside) {
            reason = "outside_project";
            explanation = "it does not resolve to a file inside the project";
        }
    }
    return rejection(std::string{renderDocumentNotWritableCode},
                     "Render document " + reference +
                         " cannot be read or restored here: " + explanation,
                     {{"method", std::string{method}},
                      {"reference", reference},
                      {"reason", reason}});
}

OrderedJson RenderConfigEditorService::getRenderDocuments(
    const Json &params) const {
    constexpr std::string_view method = "get_render_documents";
    requireOnly(params, {"references"}, method);
    std::optional<std::vector<std::string>> requested;
    if (const auto found = params.find("references"); found != params.end()) {
        if (!found->is_array())
            throw std::invalid_argument(
                "get_render_documents references must be an array of strings");
        requested.emplace();
        for (const auto &reference : *found) {
            if (!reference.is_string())
                throw std::invalid_argument(
                    "get_render_documents references must be an array of strings");
            requested->push_back(reference.get<std::string>());
        }
    }
    if (!dependencies_.resolve_authoring_context ||
        !dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path)
        return rejection("method_unavailable",
                         "get_render_documents requires the project document resolver",
                         {{"method", std::string{method}}});
    std::vector<WritableRenderDocument> writable;
    try {
        writable = writableRenderDocuments();
    } catch (const std::exception &error) {
        return preflightRejection(error, {{"participant", "pelican.render_pipeline"},
                                          {"method", std::string{method}}});
    }
    std::vector<const WritableRenderDocument *> selected;
    if (!requested) {
        for (const auto &entry : writable) selected.push_back(&entry);
    } else {
        for (const auto &reference : *requested) {
            const auto *found = findWritableRenderDocument(writable, reference);
            if (found == nullptr) {
                // RD-1b: a document inside the project that is not there now
                // (one an edit deleted, or one an undo would create) is named
                // as missing, not as a document this service does not write.
                if (isAbsentProjectDocument(reference))
                    return rejection(
                        std::string{renderDocumentMissingCode},
                        "Render document " + reference + " is not there",
                        {{"method", std::string{method}},
                         {"reference", reference}});
                return renderDocumentNotWritable(method, reference);
            }
            selected.push_back(found);
        }
    }
    auto documents = OrderedJson::array();
    for (const auto *entry : selected) {
        std::string bytes;
        try {
            bytes = readFileBytes(entry->path);
        } catch (const std::exception &error) {
            return rejection("external_modification", error.what(),
                             {{"method", std::string{method}},
                              {"reference", entry->reference}});
        }
        documents.push_back({{"reference", entry->reference},
                             {"kind", entry->kind},
                             {"digest", renderConfigSourceDigest(bytes)},
                             {"content", std::move(bytes)}});
    }
    // RR-1: whether these are the documents the live generation was composed
    // from (false on the fallback and after a refused reload).
    return {{"documents", std::move(documents)}, {"live", documentsLive()}};
}

OrderedJson RenderConfigEditorService::restoreRenderDocuments(
    const Json &params) {
    constexpr std::string_view method = "restore_render_documents";
    requireOnly(params, {"documents"}, method);
    const auto requested = params.find("documents");
    if (requested == params.end() || !requested->is_array() ||
        requested->empty())
        throw std::invalid_argument(
            "restore_render_documents requires a non-empty documents array");
    // RD-1b: base_digest null is "not there now" (the document is created),
    // content null is "delete it". Both keys are always present.
    struct Restore {
        std::string reference;
        std::optional<std::string> base_digest;
        std::optional<std::string> content;
    };
    const auto string_or_null = [&](const Json &entry, const char *field) {
        const auto found = entry.find(field);
        if (found == entry.end() || !(found->is_null() || found->is_string()))
            throw std::invalid_argument(
                std::string{"restore_render_documents document requires "} +
                field + " as a string or null");
        return found->is_null()
                   ? std::optional<std::string>{}
                   : std::optional<std::string>{found->get<std::string>()};
    };
    std::vector<Restore> restores;
    for (const auto &entry : *requested) {
        requireOnly(entry, {"reference", "base_digest", "content"},
                    "restore_render_documents document");
        Restore restore{
            .reference = requireString(entry, "reference", method),
            .base_digest = string_or_null(entry, "base_digest"),
            .content = string_or_null(entry, "content"),
        };
        if (restore.base_digest && !isLowerHexDigest(*restore.base_digest))
            throw std::invalid_argument(
                "restore_render_documents base_digest must be 64 lowercase hex characters");
        if (!restore.base_digest && !restore.content)
            throw std::invalid_argument(
                "restore_render_documents " + restore.reference +
                ": base_digest and content cannot both be null (a document "
                "that is not there cannot be deleted)");
        restores.push_back(std::move(restore));
    }
    if (!dependencies_.resolve_authoring_context ||
        !dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path)
        return rejection("method_unavailable",
                         "restore_render_documents requires the project document resolver",
                         {{"method", std::string{method}}});

    const auto gate = gateSnapshot();
    if (!gate.can_edit) {
        return rejection("gate_closed", "render config editing is currently disabled",
                         {{"method", std::string{method}},
                          {"reason", primaryReason(gate.reasons)},
                          {"gate_epoch", gate.epoch}});
    }
    std::vector<WritableRenderDocument> writable;
    try {
        writable = writableRenderDocuments();
    } catch (const std::exception &error) {
        return preflightRejection(error, {{"participant", "pelican.render_pipeline"},
                                          {"method", std::string{method}}});
    }

    // RD-1b: besides what the current composition writes, the documents the
    // current and the restored root (and each one's preset) name may be
    // created or deleted -- an authored fragment, the project preset, a
    // materialized or imported feature. The restored root is the request's
    // when it restores the root; its preset's bytes are the request's when it
    // restores the preset, else what is on disk (or the engine's).
    const auto restored_content =
        [&](const std::string &reference) -> const Restore * {
        std::string normalized;
        try {
            normalized = dependencies_.normalize_reference(reference);
        } catch (const std::exception &) {
            return nullptr;
        }
        for (const auto &restore : restores) {
            if (restore.reference.starts_with("engine://") ||
                restore.reference.find('#') != std::string::npos)
                continue;
            try {
                if (dependencies_.normalize_reference(restore.reference) ==
                    normalized)
                    return &restore;
            } catch (const std::exception &) {
            }
        }
        return nullptr;
    };
    const auto preset_on_disk =
        [&](const std::string &reference) -> std::optional<std::string> {
        if (reference.starts_with("engine://"))
            return engineResourceOrThrow(reference.substr(9));
        const auto path = dependencies_.resolve_document_path(reference);
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return std::nullopt;
        return readFileBytes(path);
    };
    const auto restored_preset =
        [&](const std::string &reference) -> std::optional<std::string> {
        if (const auto *restore = restored_content(reference))
            return restore->content;
        return preset_on_disk(reference);
    };
    std::string restored_root = document_.bytes();
    for (const auto &restore : restores) {
        const auto *found =
            findWritableRenderDocument(writable, restore.reference);
        if (found != nullptr && found->kind == "root" && restore.content)
            restored_root = *restore.content;
    }
    auto named = namedRenderDocuments(document_.bytes(), preset_on_disk);
    for (auto &entry : namedRenderDocuments(restored_root, restored_preset)) {
        if (std::none_of(named.begin(), named.end(), [&](const auto &other) {
                return other.normalized_reference == entry.normalized_reference;
            }))
            named.push_back(std::move(entry));
    }

    // Every reference first: a request that names a document this service
    // does not write is refused before any digest is compared.
    std::vector<const WritableRenderDocument *> targets;
    // RD-1b: a document is created or deleted only outside the places the
    // project keeps its code, scenes, assets and generated files. Replacing a
    // render document that is already there (RD-1) is not limited by this.
    const auto reserved_refusal = [&](const Restore &restore,
                                      const WritableRenderDocument &target)
        -> std::optional<OrderedJson> {
        if (restore.content && restore.base_digest) return std::nullopt;
        const auto location = reservedProjectLocation(target.path);
        if (!location) return std::nullopt;
        return rejection(
            std::string{renderDocumentNotWritableCode},
            "Render document " + restore.reference + " is under " + *location +
                ", where the project keeps documents that are not render "
                "documents; it is not created or deleted there",
            {{"method", std::string{method}},
             {"reference", restore.reference},
             {"reason", "reserved_location"},
             {"location", *location}});
    };
    for (const auto &restore : restores) {
        const auto *found =
            findWritableRenderDocument(writable, restore.reference);
        if (found != nullptr) {
            if (auto refused = reserved_refusal(restore, *found))
                return *refused;
        } else {
            // A document the current composition does not write, that the
            // current or the restored one names: it is either not there (and
            // is created, or the CAS below says it is gone) or a file of the
            // kind it is named as. A file of another kind -- a scene, code or
            // an asset a restored root names -- is not written or deleted.
            found = findWritableRenderDocument(named, restore.reference);
            if (found == nullptr)
                return renderDocumentNotWritable(method, restore.reference);
            if (auto refused = reserved_refusal(restore, *found))
                return *refused;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(found->path, error);
            const bool absent =
                status.type() == std::filesystem::file_type::not_found;
            // A request that says the document is not there is answered by
            // the CAS when it is; nothing is written either way.
            if (!absent && restore.base_digest) {
                bool of_kind = false;
                try {
                    of_kind = std::filesystem::is_regular_file(found->path) &&
                              isRenderDocumentOfKind(readFileBytes(found->path),
                                                     found->kind);
                } catch (const std::exception &) {
                }
                if (!of_kind)
                    return renderDocumentNotWritable(method, restore.reference);
            }
        }
        if (found->kind == "root" && !restore.content)
            return rejection(
                std::string{renderDocumentNotWritableCode},
                "Render document " + restore.reference +
                    " is the root; it is restored, never deleted",
                {{"method", std::string{method}},
                 {"reference", restore.reference},
                 {"reason", "root_document"}});
        if (std::find(targets.begin(), targets.end(), found) != targets.end())
            throw std::invalid_argument(
                "restore_render_documents names " + restore.reference +
                " more than once");
        targets.push_back(found);
    }

    // CAS: each document as it is now against the digest it was read with.
    // The root is the engine-owned document (and the file must still hold
    // it); every other document is compared on disk. The root is held to the
    // engine's document whether or not the request restores it.
    // RD-1b: a document that is not there is null on both sides.
    auto mismatches = OrderedJson::array();
    const auto digest_json = [](const std::optional<std::string> &digest) {
        return digest ? OrderedJson(*digest) : OrderedJson(nullptr);
    };
    const auto mismatch = [&](const std::string &reference,
                              const std::optional<std::string> &expected,
                              const std::optional<std::string> &provided) {
        mismatches.push_back({{"reference", reference},
                              {"expected_digest", digest_json(expected)},
                              {"provided_digest", digest_json(provided)}});
    };
    // The document as it is now: nullopt when nothing is at the path (not
    // even a dangling link), "" when something is there that does not read.
    const auto disk_digest = [](const std::filesystem::path &path)
        -> std::optional<std::string> {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (status.type() == std::filesystem::file_type::not_found)
            return std::nullopt;
        try {
            return renderConfigSourceDigest(readFileBytes(path));
        } catch (const std::exception &) {
            return std::string{};
        }
    };
    const auto root_on_disk = disk_digest(dependencies_.source_path);
    const std::optional<std::string> root_digest = document_.sourceDigest();
    bool root_restored = false;
    for (std::size_t index = 0; index < restores.size(); ++index) {
        const auto &restore = restores[index];
        const auto &target = *targets[index];
        if (target.kind == "root") {
            root_restored = true;
            if (restore.base_digest != root_digest)
                mismatch(restore.reference, root_digest, restore.base_digest);
            else if (root_on_disk != root_digest)
                mismatch(restore.reference, root_on_disk, restore.base_digest);
            continue;
        }
        const auto current = disk_digest(target.path);
        if (current != restore.base_digest)
            mismatch(restore.reference, current, restore.base_digest);
    }
    if (!root_restored && root_on_disk != root_digest)
        mismatch(dependencies_.source_reference, root_on_disk, root_digest);
    if (!mismatches.empty()) {
        const auto first = mismatches.front();
        return rejection(
            "external_modification",
            "Render document " + first.at("reference").get<std::string>() +
                " changed since it was read",
            {{"method", std::string{method}},
             {"reference", first.at("reference")},
             {"expected_digest", first.at("expected_digest")},
             {"provided_digest", first.at("provided_digest")},
             {"mismatches", std::move(mismatches)}});
    }

    // The candidate: the editor's current document set with each restored
    // document's bytes in place, each held to the digest it was read with.
    // RD-1b: a document with base_digest null is created (held to "not
    // there"), one with content null is deleted.
    auto root = document_;
    auto candidate_documents = currentDocumentSet().documents();
    for (std::size_t index = 0; index < restores.size(); ++index) {
        const auto &restore = restores[index];
        const auto &target = *targets[index];
        const auto refuse_content = [&](const std::string &message) {
            return rejection("render_pipeline_preflight_failed", message,
                             {{"participant", "pelican.render_pipeline"},
                              {"method", std::string{method}},
                              {"reference", restore.reference}});
        };
        if (!restore.content) {
            // Deleted: nothing to check but that the composition still
            // composes without it (the candidate's preflight).
        } else if (auto refused = syntaxRejection(
                       *restore.content, restore.reference, method)) {
            // BS-1b: content that is not JSON, by name.
            (*refused)["error"]["reference"] = restore.reference;
            return *refused;
        } else if (target.kind == "root") {
            try {
                root = AuthoredRenderConfigDocument::inspect(*restore.content);
            } catch (const RenderConfigLegacyRoot &error) {
                return rejection(std::string{renderConfigLegacyRootCode},
                                 error.what(),
                                 {{"method", std::string{method}},
                                  {"reference", restore.reference}});
            } catch (const std::exception &error) {
                return refuse_content(error.what());
            }
        } else if (!isRenderDocumentOfKind(*restore.content, target.kind)) {
            return refuse_content(
                "The restored " + restore.reference + " is not a " +
                std::string{renderDocumentSchema(target.kind)} + " document");
        }
        RenderConfigCandidateDocument document{
            .reference = target.reference,
            .normalized_reference = target.normalized_reference,
            .path = target.path,
            .operation = !restore.content ? RenderConfigDocumentOperation::erase
                         : !restore.base_digest
                             ? RenderConfigDocumentOperation::create
                             : RenderConfigDocumentOperation::replace,
            .expected =
                restore.base_digest
                    ? RenderConfigDocumentExpectedState{
                          RenderConfigDocumentExistence::present,
                          *restore.base_digest}
                    : RenderConfigDocumentExpectedState{
                          RenderConfigDocumentExistence::missing, {}},
            .bytes = restore.content.value_or(std::string{}),
        };
        const auto existing = std::find_if(
            candidate_documents.begin(), candidate_documents.end(),
            [&](const auto &entry) {
                return entry.normalized_reference ==
                       target.normalized_reference;
            });
        if (existing != candidate_documents.end()) *existing = std::move(document);
        else candidate_documents.push_back(std::move(document));
    }
    // As add_authored_pass and remove_authored_pass order them: a created
    // document exists before a root names it, and a deleted one goes after
    // the root has stopped naming it. The transaction is atomic either way;
    // the order is what a reader between two steps would see.
    const auto step = [](const RenderConfigCandidateDocument &document) {
        switch (document.operation) {
        case RenderConfigDocumentOperation::create: return 0;
        case RenderConfigDocumentOperation::replace: return 1;
        case RenderConfigDocumentOperation::erase: return 2;
        }
        return 1;
    };
    std::stable_sort(candidate_documents.begin(), candidate_documents.end(),
                     [&](const auto &left, const auto &right) {
                         return step(left) < step(right);
                     });
    RenderConfigCandidateDocumentSet candidate_set{
        source_normalized_reference_, std::move(candidate_documents)};

    // G-4: material_route_default is read once per engine start; a restore
    // that changes it says so, as add_authored_pass does.
    bool restart_required = false;
    try {
        restart_required =
            heldRouteDefault(document_.bytes()) !=
            readMaterialRouteDefault(
                renderingSectionOrEmpty(Json::parse(root.bytes())));
    } catch (const std::exception &error) {
        return preflightRejection(error, {{"participant", "pelican.render_pipeline"},
                                          {"method", std::string{method}}});
    }
    auto result = enqueueAuthoredPassTicket(
        std::string{method}, gate, document_.sourceDigest(), std::move(root),
        std::move(candidate_set), std::string{},
        /*edits_additional_documents=*/true, {}, restart_required);
    if (result.value("status", std::string{}) == "accepted")
        pending_.back().reload_managed_baselines = true;
    return result;
}

void RenderConfigEditorService::rememberWrittenDocuments(
    const RenderConfigCandidateDocumentSet &documents) {
    for (const auto &document : documents.documents()) {
        if (document.normalized_reference == source_normalized_reference_)
            continue;
        if (document.operation == RenderConfigDocumentOperation::erase)
            known_document_digests_.erase(document.normalized_reference);
        else
            known_document_digests_.insert_or_assign(
                document.normalized_reference,
                renderConfigSourceDigest(document.bytes));
    }
}

void RenderConfigEditorService::verifyReloadedDocuments(
    const RenderConfigCandidateDocumentSet &documents) {
    for (const auto &document : documents.documents()) {
        std::string actual;
        try {
            actual = renderConfigSourceDigest(readFileBytes(document.path));
        } catch (const std::exception &error) {
            throw RenderConfigExternalModification(
                "Render document " + document.reference +
                " could not be read again before the reload was published: " +
                error.what());
        }
        if (actual != document.expected.digest)
            throw RenderConfigExternalModification(
                "Render document " + document.reference +
                " changed again after reload_render_documents read it");
    }
}

void RenderConfigEditorService::noteProblems(
    bool reload, bool source_only, const OrderedJson &result) const {
    const auto status = result.value("status", std::string{});
    if (status == "committed") {
        // A reload takes in what is on disk; an edit that published a new
        // generation composed every render document it reads. An edit that
        // published nothing (no_change, or only authored bytes the engine
        // never reads) composed nothing.
        if (reload || (!source_only && !result.value("no_change", false)))
            clearRenderPipelineProblems();
        return;
    }
    if (!reload || !refusesDocuments(result)) return;
    recordRenderPipelineProblem(
        problemSourceReloadRpc,
        Json::parse(result.at("error").dump()), dependencies_.source_reference);
}

bool RenderConfigEditorService::refusesDocuments(const OrderedJson &result) {
    if (result.value("status", std::string{}) != "rejected") return false;
    const auto error = result.find("error");
    if (error == result.end() || !error->is_object()) return false;
    const auto code = error->value("code", std::string{});
    // Refusals of the moment, not of the documents.
    return !(code == reloadBusyCode || code == "gate_closed" ||
             code == "external_modification" ||
             code == "stale_source_digest");
}

void RenderConfigEditorService::holdRefusedDocuments(
    AuthoredRenderConfigDocument root,
    const std::map<std::string, std::string> &digests, bool unparsed) {
    if (!unparsed) {
        readable_document_.reset();
    } else if (!readable_document_ && Json::accept(document_.bytes())) {
        // The root held until now stays what the structural reads answer
        // from, with its managed fragments (a root that is not JSON names
        // none).
        readable_document_ = document_;
        readable_document_live_ = documentsLive();
    }
    document_ = std::move(root);
    for (const auto &[normalized, digest] : digests)
        if (normalized != source_normalized_reference_)
            known_document_digests_.insert_or_assign(normalized, digest);
    holds_refused_documents_ = true;
    // The managed fragments the held root names, as they are on disk.
    if (!unparsed) loadManagedDocumentBaselines();
}

const AuthoredRenderConfigDocument &
RenderConfigEditorService::readDocument() const {
    return readable_document_ ? *readable_document_ : document_;
}

bool RenderConfigEditorService::readLive() const {
    return readable_document_ ? readable_document_live_ : documentsLive();
}

OrderedJson RenderConfigEditorService::markRead(OrderedJson read) const {
    if (readable_document_) read["held_unparsed"] = true;
    return read;
}

void RenderConfigEditorService::noteHeldDocuments(const Ticket &ticket,
                                                  const OrderedJson &result) {
    const auto status = result.value("status", std::string{});
    if (status == "committed") {
        // Whatever committed is JSON: the editor holds a root that reads.
        readable_document_.reset();
        // As noteProblems clears the render pipeline's problems.
        if (ticket.reload ||
            (!ticket.source_only && !result.value("no_change", false)))
            holds_refused_documents_ = false;
        return;
    }
    if (ticket.reload && refusesDocuments(result)) {
        try {
            holdRefusedDocuments(ticket.candidate, ticket.reload_digests);
        } catch (const std::exception &) {
            // The refusal is the answer either way; the editor keeps the
            // documents it held.
        }
    }
}

bool RenderConfigEditorService::documentsLive() const {
    if (holds_refused_documents_) return false;
    try {
        return !(dependencies_.runtime_fallback &&
                 dependencies_.runtime_fallback());
    } catch (const std::exception &) {
        return true;
    }
}

OrderedJson RenderConfigEditorService::reloadRenderDocuments(
    const Json &params, bool preview_open) {
    auto answer = reloadRenderDocumentsUnnoted(params, preview_open);
    // A reload refused at once; one accepted is noted when its ticket ends.
    if (answer.value("status", std::string{}) == "rejected")
        noteProblems(true, false, answer);
    return answer;
}

OrderedJson RenderConfigEditorService::reloadRenderDocumentsUnnoted(
    const Json &params, bool preview_open) {
    constexpr std::string_view method = "reload_render_documents";
    requireOnly(params, {}, method);
    // Taking in what is on disk and an edit of what was there before must
    // not interleave: an edit queued before the reload would commit over it.
    if (preview_open || !pending_.empty() || inflight_ticket_) {
        auto tickets = OrderedJson::array();
        if (inflight_ticket_) tickets.push_back(*inflight_ticket_);
        for (const auto &ticket : pending_)
            if (!inflight_ticket_ || ticket.id != *inflight_ticket_)
                tickets.push_back(ticket.id);
        const bool ticket_pending = !tickets.empty();
        return rejection(
            std::string{reloadBusyCode},
            ticket_pending
                ? "render documents cannot be reloaded while a render edit "
                  "is pending; reload after its result arrives"
                : "render documents cannot be reloaded while a scene preview "
                  "is open",
            {{"method", std::string{method}},
             {"reason", ticket_pending ? "ticket_pending" : "preview_open"},
             {"tickets", std::move(tickets)}});
    }
    const auto gate = gateSnapshot();
    if (!gate.can_edit) {
        return rejection("gate_closed",
                         "render config editing is currently disabled",
                         {{"method", std::string{method}},
                          {"reason", primaryReason(gate.reasons)},
                          {"gate_epoch", gate.epoch}});
    }
    const auto preflight_details = [&](const std::string &reference = {}) {
        OrderedJson details{{"participant", "pelican.render_pipeline"},
                            {"method", std::string{method}}};
        if (!reference.empty()) details["reference"] = reference;
        return details;
    };

    // The root as it is on disk. A root that does not read or parse is
    // refused here, as restore_render_documents refuses such content.
    std::string root_bytes;
    try {
        root_bytes = readFileBytes(dependencies_.source_path);
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what(),
                         preflight_details(dependencies_.source_reference));
    }
    // BS-1b: one that is not JSON, by name. RR-1: held as its bytes, as an
    // engine on its fallback holds it, so a restore on its digest replaces
    // it.
    if (auto refused = syntaxRejection(
            root_bytes, dependencies_.source_reference, method)) {
        holdRefusedDocuments(AuthoredRenderConfigDocument::unparsed(root_bytes),
                             {}, /*unparsed=*/true);
        return *refused;
    }
    // A root that is JSON but not a pelican.graph object is refused and not
    // held: the editor holds a graph root or bytes that are not JSON.
    std::optional<AuthoredRenderConfigDocument> root;
    try {
        root = AuthoredRenderConfigDocument::inspect(root_bytes);
    } catch (const RenderConfigLegacyRoot &error) {
        return rejection(std::string{renderConfigLegacyRootCode}, error.what(),
                         {{"method", std::string{method}},
                          {"reference", dependencies_.source_reference}});
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what(),
                         preflight_details(dependencies_.source_reference));
    }

    // The candidate: the root and the managed fragments it names, then every
    // other render document the composition reads, each as it is on disk and
    // held to that digest. The runtime composes these bytes, and the source
    // commit only checks that they are still what is on disk.
    std::vector<RenderConfigCandidateDocument> documents;
    const auto add_document = [&](const std::string &reference,
                                  const std::string &normalized,
                                  const std::filesystem::path &path,
                                  std::string bytes) {
        const auto digest = renderConfigSourceDigest(bytes);
        documents.push_back(RenderConfigCandidateDocument{
            .reference = reference,
            .normalized_reference = normalized,
            .path = path,
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present, digest},
            .bytes = std::move(bytes),
        });
    };
    // RR-1: a refusal of what was read makes the root and every document
    // read so far the editor's base.
    const auto held_refusal = [&](OrderedJson refusal) {
        std::map<std::string, std::string> read;
        for (const auto &document : documents)
            read.insert_or_assign(document.normalized_reference,
                                  document.expected.digest);
        holdRefusedDocuments(*root, read);
        return refusal;
    };
    add_document(dependencies_.source_reference, source_normalized_reference_,
                 dependencies_.source_path, root_bytes);
    if (dependencies_.normalize_reference && dependencies_.resolve_document_path) {
        for (const auto &reference : allFeatureReferences(root_bytes)) {
            try {
                const auto path = dependencies_.resolve_document_path(reference);
                if (!isManagedFragmentPath(dependencies_.project_root, path))
                    continue;
                std::error_code error;
                if (!std::filesystem::is_regular_file(path, error) || error)
                    continue;
                auto bytes = readFileBytes(path);
                if (!hasManagedFragmentMarker(bytes)) continue;
                const auto normalized =
                    dependencies_.normalize_reference(reference);
                if (std::any_of(documents.begin(), documents.end(),
                                [&](const auto &entry) {
                                    return entry.normalized_reference ==
                                           normalized;
                                }))
                    continue;
                add_document(reference, normalized, path, std::move(bytes));
            } catch (const std::exception &) {
                // As loadManagedDocumentBaselines: not a managed fragment.
            }
        }
    }
    std::vector<WritableRenderDocument> render_documents;
    try {
        render_documents = renderDocumentsOf(
            RenderConfigCandidateDocumentSet{source_normalized_reference_,
                                             documents},
            root_bytes);
    } catch (const std::exception &error) {
        return held_refusal(preflightRejection(error, preflight_details()));
    }

    std::map<std::string, std::string> published;
    if (dependencies_.published_documents) {
        try {
            published = dependencies_.published_documents();
        } catch (const std::exception &) {
            // Without the live generation's documents, a document this
            // editor has not written reads as changed.
        }
    }
    auto listed = OrderedJson::object();
    auto changed = OrderedJson::array();
    std::map<std::string, std::string> digests;
    for (const auto &entry : render_documents) {
        auto index = static_cast<std::size_t>(
            std::find_if(documents.begin(), documents.end(),
                         [&](const auto &document) {
                             return document.normalized_reference ==
                                    entry.normalized_reference;
                         }) -
            documents.begin());
        if (index == documents.size()) {
            try {
                add_document(entry.reference, entry.normalized_reference,
                             entry.path, readFileBytes(entry.path));
            } catch (const std::exception &error) {
                return held_refusal(
                    rejection("render_pipeline_preflight_failed", error.what(),
                              preflight_details(entry.reference)));
            }
        }
        const std::string digest = documents[index].expected.digest;
        listed[entry.reference] = digest;
        digests.insert_or_assign(entry.normalized_reference, digest);
        std::optional<std::string> previous;
        if (entry.kind == "root") {
            previous = document_.sourceDigest();
        } else if (const auto known =
                       known_document_digests_.find(entry.normalized_reference);
                   known != known_document_digests_.end()) {
            previous = known->second;
        } else if (const auto managed =
                       managed_documents_.find(entry.normalized_reference);
                   managed != managed_documents_.end()) {
            previous = managed->second.digest;
        } else if (const auto live = published.find(entry.normalized_reference);
                   live != published.end()) {
            previous = live->second;
        }
        if (previous != digest) changed.push_back(entry.reference);
    }
    // IA-2: an initial asset rewritten on disk is a change though every
    // document is the same: the new generation reads it again and its first
    // frame copies it in (IA-3: a value.sample_table's table or its manifest,
    // which the new generation's value graph reads again). One that cannot be
    // read now counts as changed, and the composition refuses it as it would
    // with a document edit.
    auto changed_assets = OrderedJson::array();
    if (dependencies_.published_assets) {
        std::vector<RenderConfigPublishedAsset> assets;
        try {
            assets = dependencies_.published_assets();
        } catch (const std::exception &) {
        }
        std::map<std::string, bool> asset_changed;
        for (const auto &asset : assets) {
            bool differs = true;
            try {
                differs = assetFileSha256(asset.file) != asset.sha256;
            } catch (const std::exception &) {
            }
            asset_changed[asset.asset] = asset_changed[asset.asset] || differs;
        }
        for (const auto &[asset, differs] : asset_changed)
            if (differs) changed_assets.push_back(asset);
    }
    // BS-1: on the engine's fallback nothing on disk is live, so the
    // documents are composed whether or not they changed. RR-1: nor is what
    // the editor holds after a refused reload.
    const bool no_change =
        documentsLive() && changed.empty() && changed_assets.empty();

    // G-4: material_route_default is read once per engine start.
    bool restart_required = false;
    try {
        restart_required =
            heldRouteDefault(document_.bytes()) !=
            readMaterialRouteDefault(
                renderingSectionOrEmpty(Json::parse(root->bytes())));
    } catch (const std::exception &error) {
        return held_refusal(preflightRejection(error, preflight_details()));
    }
    // G-6: every variant the engine registered composes it first.
    if (!no_change) {
        if (auto refused = preflightGraphVariants(
                RenderConfigCandidateDocumentSet{source_normalized_reference_,
                                                 documents},
                preflight_details()))
            return held_refusal(std::move(*refused));
    }
    RenderConfigCandidateDocumentSet candidate_set{
        source_normalized_reference_, std::move(documents)};

    const auto ticket_id =
        "render-config-reload-" + std::to_string(next_ticket_++);
    pending_.push_back(Ticket{
        .id = ticket_id,
        .accepted_gate_epoch = gate.epoch,
        .base_source_digest = document_.sourceDigest(),
        .candidate = std::move(*root),
        .candidate_documents = std::move(candidate_set),
        .operation = std::string{method},
        // The root may be byte-identical while a preset or feature changed.
        .edits_additional_documents = true,
        .restart_required = restart_required,
        .reload_managed_baselines = true,
        .reload = true,
        .reload_no_change = no_change,
        .reload_documents = std::move(listed),
        .reload_changed = std::move(changed),
        .reload_digests = std::move(digests),
        .reload_changed_assets = std::move(changed_assets),
    });
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return {{"ticket", ticket_id},
            {"status", "accepted"},
            {"base_source_digest", digestJson(document_.sourceDigest())}};
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
        noteProblems(transaction->ticket.reload,
                     transaction->ticket.source_only, result);
        noteHeldDocuments(transaction->ticket, result);
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
        // RS-1: the runtime refused the candidate only because a live
        // material would be drawn with another physical rendering contract.
        // The documents are committed for the next engine start (the same
        // preset CAS and the same multi-file commit the runtime would have
        // called), the live engine keeps its generation, and the result
        // says restart_required. Any other failure is refused below.
        std::optional<OrderedJson> restart_reason;
        if (!applied.staged && !applied.committed &&
            !transaction->source_commit_called &&
            transaction->ticket.candidate_documents) {
            restart_reason =
                physicalContractRestartReason(applied.exception);
            if (restart_reason) {
                const auto runtime = dependencies_.runtime_snapshot();
                try {
                    if (transaction->ticket.reload) {
                        // RL-1: what is on disk is already the next start's;
                        // nothing is written.
                        transaction->source_commit_called = true;
                        verifyReloadedDocuments(
                            *transaction->ticket.candidate_documents);
                    } else {
                        if (transaction->verify_preset)
                            transaction->verify_preset();
                        transaction->source_commit_called = true;
                        transaction->source_receipt =
                            commitRenderConfigCandidateDocuments(
                                dependencies_.project_root,
                                *transaction->ticket.candidate_documents);
                    }
                } catch (const RenderConfigExternalModification &error) {
                    finish(rejection(
                        "external_modification", error.what(),
                        {{"ticket", transaction->ticket.id},
                         {"expected_source_digest",
                          transaction->ticket.base_source_digest}}));
                    return;
                }
                applied.committed = true;
                applied.published_generation =
                    runtime.published_generation;
                applied.post_commit_error.clear();
            }
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
                if (auto refusal = graphRefusalOf(applied.exception)) {
                    for (const auto &[key, value] :
                         refusal->second.items()) {
                        if (!details.contains(key)) details[key] = value;
                    }
                    // CT-2: judged against the root this ticket carried.
                    details = withGraphRefusalDocument(
                        std::move(details), refusal->first,
                        transaction->ticket.candidate.bytes(),
                        dependencies_.source_reference);
                    finish(rejection(refusal->first, applied.error,
                                     std::move(details)));
                    return;
                }
                // EL-2: a physical fragment the linker found stale.
                if (auto refusal = physicalFragmentStaleRefusal(
                        applied.exception,
                        transaction->ticket.candidate.bytes(), details)) {
                    finish(std::move(*refusal));
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
        // RL-1: a reload wrote nothing; its documents are every render
        // document it read.
        const auto written_documents =
            transaction->ticket.reload
                ? transaction->ticket.reload_documents
            : transaction->ticket.candidate_documents
                ? writtenDocumentDigests(*transaction->ticket.candidate_documents)
                : OrderedJson::object();
        if (transaction->ticket.reload) {
            for (const auto &[normalized, digest] :
                 transaction->ticket.reload_digests)
                if (normalized != source_normalized_reference_)
                    known_document_digests_.insert_or_assign(normalized,
                                                             digest);
        } else if (transaction->ticket.candidate_documents) {
            rememberWrittenDocuments(*transaction->ticket.candidate_documents);
        }
        if (transaction->ticket.reload_managed_baselines) {
            // RD-1: a restore may have changed which fragments the root
            // names; read them again from what was just written.
            loadManagedDocumentBaselines();
        } else if (transaction->ticket.candidate_documents) {
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
            // RD-1: what this commit wrote, {reference: sha256 hex}.
            {"documents", written_documents},
        };
        if (!transaction->ticket.operation.empty()) {
            completed["operation"] =
                transaction->ticket.operation;
        }
        if (!transaction->ticket.fragment_reference.empty()) {
            completed["fragment_reference"] =
                transaction->ticket.fragment_reference;
        }
        if (transaction->ticket.reload) {
            completed["changed"] = transaction->ticket.reload_changed;
            completed["changed_assets"] =
                transaction->ticket.reload_changed_assets;
        }
        if (!applied.post_commit_error.empty()) {
            completed["post_commit_error"] =
                applied.post_commit_error;
        }
        // PV-1: whether the generation this edit published has a preview,
        // and why not (an edit preview cannot take is not refused).
        try {
            if (dependencies_.runtime_snapshot) {
                auto preview = dependencies_.runtime_snapshot().preview;
                if (!preview.is_null()) completed["preview"] = std::move(preview);
            }
        } catch (const std::exception &) {
        }
        for (const auto &[key, value] :
             transaction->ticket.result_extras.items()) {
            if (!completed.contains(key)) completed[key] = value;
        }
        if (restart_reason) {
            // RS-1: committed, not live; the next start builds it.
            completed["restart_required"] = true;
            completed["restart_reason"] = std::move(*restart_reason);
        } else if (transaction->ticket.restart_required) {
            // G-4: material_route_default is read once per engine start.
            completed["restart_required"] = true;
            completed["restart_reason"] =
                OrderedJson{{"code", "material_route_default_changed"}};
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
    // AB-1: a finished bake writes no render document, so it ends whatever
    // ticket is in flight.
    harvestAudioBakes();
    if (inflight_ticket_) return;
    // FG-2: a verify that passed queues its mark ahead of the queue below.
    harvestFoldVerifies();
    auto queue = std::move(pending_);
    pending_.clear();
    for (auto ticket_iterator = queue.begin();
         ticket_iterator != queue.end(); ++ticket_iterator) {
        auto &ticket = *ticket_iterator;
        auto finish = [&](OrderedJson result) {
            if (!result.contains("ticket")) result["ticket"] = ticket.id;
            noteProblems(ticket.reload, ticket.source_only, result);
            noteHeldDocuments(ticket, result);
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
            // RL-1: a reload expects the disk to hold what it read, which is
            // not the editor's document.
            const auto expected_disk_digest =
                ticket.reload ? ticket.candidate.sourceDigest()
                              : ticket.base_source_digest;
            if (disk_digest != expected_disk_digest) {
                finish(rejection(
                    "external_modification",
                    ticket.reload
                        ? "render config source changed again after "
                          "reload_render_documents read it"
                        : "render config source changed outside the editor",
                    {{"ticket", ticket.id},
                     {"expected_source_digest", expected_disk_digest},
                     {"actual_source_digest", disk_digest}}));
                continue;
            }

            // RL-1: a reload that found every document as the editor knew
            // it publishes nothing.
            if (ticket.reload && ticket.reload_no_change) {
                const auto runtime = dependencies_.runtime_snapshot();
                for (const auto &[normalized, digest] : ticket.reload_digests)
                    if (normalized != source_normalized_reference_)
                        known_document_digests_.insert_or_assign(normalized,
                                                                 digest);
                finish(withPublishedPreview(OrderedJson{
                    {"ticket", ticket.id},
                    {"status", "committed"},
                    {"committed", true},
                    {"no_change", true},
                    {"operation", ticket.operation},
                    {"published_generation", runtime.published_generation},
                    {"source_digest", digestJson(document_.sourceDigest())},
                    {"source_commit_called", false},
                    {"documents", ticket.reload_documents},
                    {"changed", OrderedJson::array()},
                    {"changed_assets", OrderedJson::array()},
                }, runtime.preview));
                continue;
            }

            // A preset edit can change another document while the root stays
            // byte-identical. It must still compile, check its CAS and publish.
            if (!ticket.edits_additional_documents && ticket.candidate.sourceDigest() ==
                document_.sourceDigest()) {
                const auto runtime =
                    dependencies_.runtime_snapshot();
                finish(withPublishedPreview(OrderedJson{
                    {"ticket", ticket.id},
                    {"status", "committed"},
                    {"committed", true},
                    {"no_change", true},
                    {"published_generation",
                     runtime.published_generation},
                    {"source_digest",
                     digestJson(document_.sourceDigest())},
                    {"source_commit_called", false},
                    // RD-1: nothing was written.
                    {"documents", OrderedJson::object()},
                }, runtime.preview));
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
                auto written_documents =
                    writtenDocumentDigests(*ticket.candidate_documents);
                rememberWrittenDocuments(*ticket.candidate_documents);
                document_ = std::move(ticket.candidate);
                const auto runtime = dependencies_.runtime_snapshot();
                finish(withPublishedPreview(OrderedJson{
                    {"ticket", ticket.id},
                    {"status", "committed"},
                    {"committed", true},
                    // Said out loud, because it is the whole point: the drawn
                    // graph was not recomposed for this edit.
                    {"recomposed", false},
                    {"published_generation", runtime.published_generation},
                    {"source_digest", digestJson(document_.sourceDigest())},
                    {"source_commit_called", true},
                    {"documents", std::move(written_documents)},
                }, runtime.preview));
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
            transaction->verify_preset = verify_preset;
            // RL-1: a reload writes nothing; its source commit is the check
            // that every document is still what was read.
            const auto reload_commit = [transaction] {
                transaction->source_commit_called = true;
                try {
                    verifyReloadedDocuments(
                        *transaction->ticket.candidate_documents);
                } catch (const RenderConfigExternalModification &error) {
                    transaction->source_external_modification = error.what();
                    throw;
                }
            };
            const auto edit_commit =
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
            const RenderConfigSourceCommit source_commit =
                transaction->ticket.reload
                    ? RenderConfigSourceCommit{reload_commit}
                    : RenderConfigSourceCommit{edit_commit};
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
