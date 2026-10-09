#include "projectmaterialasset.hpp"

#include "../../project/assetdataformat.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/materialformat.hpp"
#include "../../project/renderconfigdocument.hpp"
#include "../../project/materiallowering.hpp"
#include "../../project/projectdocumenttransaction.hpp"
#include "../../project/surfaceformat.hpp"
#include "../asset/model.hpp"
#include "../launchconfig.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/projectsrc.hpp"
#include "../loader/resolvedscene.hpp"
#include "../log.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../shader/passcontractrefusal.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../vkcore/core.hpp"
#include "../watch/assetkey.hpp"
#include "../watch/problemlist.hpp"
#include "../watch/reloadservice.hpp"
#include "materialcontainer.hpp"
#include "material.hpp"
#include "materialshaderconfig.hpp"
#include "projectmaterialtextureresolver.hpp"
#include "standardmaterialresource.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

struct SourceDocument {
    std::string reference;
    std::filesystem::path path;
    watch::AssetKey key;
    nlohmann::json json;
    MaterialFormatDocument parsed;
};

struct PreparedProjectMaterial {
    std::size_t document_index = 0;
    MaterialDefinition definition;
    LoweredMaterial lowered;
    std::vector<LoweredNamedMaterialVariant> variants;
};

nlohmann::json readJsonFile(
    const std::filesystem::path &path,
    std::string_view context) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error(
            std::string{context} +
            " is missing: " + path.string());
    }
    try {
        return nlohmann::json::parse(input);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            std::string{context} + " '" +
            path.string() + "': " + error.what());
    }
}

std::set<std::string> referencedSurfaces(
    const nlohmann::json &document) {
    std::set<std::string> result;
    const auto materials = document.find("materials");
    if (materials == document.end() ||
        !materials->is_array()) {
        return result;
    }
    for (const auto &material : *materials) {
        if (!material.is_object()) {
            continue;
        }
        if (const auto surface =
                material.find("surface");
            surface != material.end() &&
            surface->is_string()) {
            result.insert(surface->get<std::string>());
        }
        const auto variants = material.find("variants");
        if (variants == material.end() ||
            !variants->is_object()) {
            continue;
        }
        for (const auto &[name, variant] :
             variants->items()) {
            (void)name;
            if (!variant.is_object()) {
                continue;
            }
            if (const auto surface =
                    variant.find("surface");
                surface != variant.end() &&
                surface->is_string()) {
                result.insert(
                    surface->get<std::string>());
            }
        }
    }
    return result;
}

GlobalTextureId resolveOptionalTexture(
    ProjectMaterialTextureResolver &resolver,
    const std::optional<std::string> &reference,
    GlobalTextureId fallback,
    SurfaceTextureRole role) {
    return reference
               ? resolver.resolve(*reference, role)
               : fallback;
}

float rayAlphaCutoff(const LoweredMaterial &lowered) {
    const auto found = std::find_if(
        lowered.values_layout.members.begin(),
        lowered.values_layout.members.end(),
        [](const auto &member) { return member.name == "alpha_cutoff"; });
    if (found == lowered.values_layout.members.end() ||
        found->size < sizeof(float) ||
        found->offset > lowered.values.size() ||
        sizeof(float) > lowered.values.size() - found->offset) {
        return 0.5F;
    }
    float result = 0.5F;
    std::memcpy(&result, lowered.values.data() + found->offset,
                sizeof(result));
    return result;
}

MaterialInfo makeMaterialInfo(
    const MaterialBase &base,
    const LoweredMaterial &lowered,
    const SurfaceFormatDocument &surface,
    std::string_view surface_reference,
    std::span<const std::string> shader_defines,
    StandardMaterialResource &standard,
    ShaderLibrary &shaders,
    ProjectMaterialTextureResolver &textures) {
    const auto bundles =
        shaders.loadFromSurfaceForMaterial(
            surface, surface_reference, lowered,
            {shader_defines.begin(),
             shader_defines.end()});
    std::vector<std::string> skinned_defines{shader_defines.begin(), shader_defines.end()};
    skinned_defines.emplace_back("PELICAN_SKINNED");
    const auto skinned_bundles = shaders.loadFromSurfaceForMaterial(
        surface, surface_reference, lowered, std::move(skinned_defines));
    MaterialInfo info{
        .vert_shader = bundles.vertex,
        .frag_shader = bundles.fragment,
        .skinned_vert_shader = skinned_bundles.vertex,
        .base_color_texture =
            resolveOptionalTexture(
                textures, base.base_color_texture,
                standard.whiteTexture(),
                SurfaceTextureRole::color),
        .metallic_roughness_texture =
            resolveOptionalTexture(
                textures,
                base.metallic_roughness_texture,
                standard
                    .metallicRoughnessDefaultTexture(),
                SurfaceTextureRole::data),
        .normal_texture =
            resolveOptionalTexture(
                textures, base.normal_texture,
                standard.normalDefaultTexture(),
                SurfaceTextureRole::data),
        .emissive_texture =
            resolveOptionalTexture(
                textures, base.emissive_texture,
                standard.whiteTexture(),
                SurfaceTextureRole::color),
        .occlusion_texture =
            resolveOptionalTexture(
                textures, base.occlusion_texture,
                standard.occlusionDefaultTexture(),
                SurfaceTextureRole::data),
        .base_color_texture_identity =
            base.base_color_texture.value_or(std::string{}),
        .alpha_mode = lowered.routing
                          ? lowered.routing->alpha_mode
                          : MaterialAlphaMode::opaque,
        .alpha_cutoff = rayAlphaCutoff(lowered),
        .base_color_factor =
            glm::vec4{
                static_cast<float>(
                    base.base_color_factor[0]),
                static_cast<float>(
                    base.base_color_factor[1]),
                static_cast<float>(
                    base.base_color_factor[2]),
                static_cast<float>(
                    base.base_color_factor[3]),
            },
        .emissive_factor =
            glm::vec3{
                static_cast<float>(
                    base.emissive_factor[0]),
                static_cast<float>(
                    base.emissive_factor[1]),
                static_cast<float>(
                    base.emissive_factor[2]),
            },
        .metallic_factor =
            static_cast<float>(
                base.metallic_factor),
        .roughness_factor =
            static_cast<float>(
                base.roughness_factor),
    };
    applyLoweredMaterialForRoute(
        info, lowered,
        [&](std::string_view reference,
            SurfaceTextureRole role) {
            return textures.resolve(reference, role);
        });
    return info;
}

} // namespace

struct ProjectMaterialAssetContainer::Impl {
    std::vector<ModelTemplate::NamedMaterial>
        named_materials;
    // MT-1: the document each named material was read from (its asset_data
    // materials[] reference), by name.
    std::map<std::string, std::string>
        material_documents;
    std::unique_ptr<ProjectMaterialTextureResolver>
        texture_resolver;

    // Reads, lowers and registers every material of `references` (asset_data
    // materials[] paths). A document that does not parse, a surface that is
    // missing, a name another document (or one already registered) holds:
    // all of it is refused before the first material is registered.
    // MT-3 `replace`: the documents are registered already; their materials
    // are registered anew and take the names over (the names they held go
    // to `previous`, still registered: the caller releases them once
    // nothing draws with them).
    std::vector<std::string> registerDocuments(
        std::span<const std::string> references,
        bool replace = false,
        std::vector<GlobalMaterialId> *previous = nullptr);

    // FO-1: the documents refused at start, by reference, and the models
    // whose bindings kept their own material for want of them.
    std::map<std::string, RefusedDocument, std::less<>> refused;
    std::map<std::string, std::vector<std::string>> unresolved_bindings;
    // EB-2: registered documents whose last values reload did not read, by
    // reference.
    std::map<std::string, RefusedDocument, std::less<>> reload_failures;
    // EB-6: moves whenever the registered names change.
    std::uint64_t names_generation = 0;
    // EB-6: the scene objects whose own material nothing registered holds,
    // and the scene projection and names generation they were found with.
    struct UnresolvedObjectMaterial {
        std::uint64_t object = 0;
        std::optional<std::string> name;
        std::string scene_id;
        std::string material;
        std::string model;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> primitive;
        friend bool operator==(const UnresolvedObjectMaterial &, const UnresolvedObjectMaterial &) = default;
    };
    std::vector<UnresolvedObjectMaterial> unresolved_objects;
    std::optional<std::string> scene_document;
    std::shared_ptr<const void> objects_scene;
    std::uint64_t objects_names_generation = 0;
    // EB-2: the registered document (or the refused one) the key names.
    std::optional<std::string> registeredReferenceOf(const watch::AssetKey &key) const;
    // The material problems as they are now, in the problem list.
    void publishProblems() const;
};

namespace {

// FO-1: what a listed document that did not register is: not there, not
// JSON (where), or what registering it said.
ProjectMaterialAssetContainer::RefusedDocument refusedDocumentOf(
    const std::string &reference, std::string registration_error) {
    ProjectMaterialAssetContainer::RefusedDocument refused{
        .status = "invalid",
        .code = "material.document_invalid@1",
        .reason = std::move(registration_error),
    };
    std::string bytes;
    try {
        const auto path = GET_MODULE(PathResolver).resolveExistingFile(reference);
        std::ifstream input{path, std::ios::binary};
        if (!input.is_open()) throw std::runtime_error("project material document is missing: " + path.string());
        bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    } catch (const std::exception &error) {
        refused.status = "missing";
        refused.code = "material.document_unknown@1";
        refused.reason = error.what();
        return refused;
    }
    refused.digest = projectDocumentDigest(bytes);
    if (const auto syntax = renderDocumentSyntax(bytes, reference)) {
        refused.reason = syntax->value("reason", refused.reason);
        if (syntax->contains("line")) refused.line = syntax->at("line").get<std::uint64_t>();
        if (syntax->contains("column")) refused.column = syntax->at("column").get<std::uint64_t>();
    }
    return refused;
}

// `after_start` (EB-2): a registered document a values reload could not read;
// the engine keeps what it read before.
nlohmann::json documentProblem(const std::string &reference,
                               const ProjectMaterialAssetContainer::RefusedDocument &refused,
                               bool after_start = false) {
    const auto source = !refused.source.empty() ? refused.source
                        : after_start           ? std::string{problemSourceHotReload}
                                                : std::string{problemSourceStartup};
    if (refused.status == "unusable") {
        nlohmann::json problem{
            {"severity", std::string{problemSeverityError}},
            {"source", source},
            {"code", refused.code},
            {"message", materialPassContractUnmetMessage(refused)},
            {"fields", materialPassContractUnmetFields(reference, refused)},
        };
        // Where it is fixed: the document that declares the pass.
        if (refused.pass_document) {
            problem["document"] = *refused.pass_document;
            if (refused.pass_document_kind) problem["document_kind"] = *refused.pass_document_kind;
        }
        if (refused.pass_node) problem["node"] = *refused.pass_node;
        return problem;
    }
    nlohmann::json fields{{"path", reference}, {"reason", refused.reason}};
    if (refused.digest) fields["digest"] = *refused.digest;
    nlohmann::json problem{
        {"severity", std::string{problemSeverityError}},
        {"source", source},
        {"code", refused.code},
        {"message", refused.code + ": the material document '" + reference + "' " +
                        (refused.status == "missing" ? "is missing" : "could not be read") +
                        (after_start ? "; the engine keeps drawing its materials as it read them before: "
                                     : "; the engine started without its materials: ") +
                        refused.reason},
        {"document_kind", std::string{problemKindMaterial}},
    };
    if (const auto document = graphDocumentReference(reference)) problem["document"] = *document;
    if (refused.line) {
        problem["line"] = *refused.line;
        fields["line"] = *refused.line;
    }
    if (refused.column) {
        problem["column"] = *refused.column;
        fields["column"] = *refused.column;
    }
    problem["fields"] = std::move(fields);
    return problem;
}

} // namespace

ProjectMaterialAssetContainer::RefusedDocument materialPassContractRefusal(
    const std::string &reference, const PassContractRefusal &refusal) {
    (void)reference;
    ProjectMaterialAssetContainer::RefusedDocument refused{
        .status = "unusable",
        .code = std::string{materialPassContractUnmetCode},
        .reason = refusal.what(),
        .material = refusal.material(),
        .requirement = refusal.requirement(),
    };
    const auto *passes = FastModuleContainer::tryGet<RenderingPassContainer>();
    std::optional<MaterialPassDeclaration> first;
    for (const auto &pass : refusal.passes()) {
        auto name = pass;
        if (passes != nullptr)
            if (auto declaration = passes->materialPassDeclaration(pass)) {
                name = declaration->name;
                if (!first) first = std::move(declaration);
            }
        if (std::find(refused.passes.begin(), refused.passes.end(), name) == refused.passes.end())
            refused.passes.push_back(std::move(name));
    }
    if (!first) return refused;
    // Where the pass is declared, read as get_compute_task_authoring reads a
    // task's: a feature's own document, the root for a node of its graph, the
    // preset the root selects, or else the root's rendering section.
    std::string root;
    if (const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>())
        root = config->renderingConfigReference();
    std::string document;
    std::string_view kind;
    if (first->source == "feature") {
        document = first->provider_reference;
        kind = problemKindFeature;
    } else if (first->graph_node) {
        document = root;
        kind = problemKindRootGraph;
        refused.pass_node = *first->graph_node;
    } else if (first->preset_reference) {
        document = *first->preset_reference;
        kind = problemKindPreset;
    } else {
        document = root;
        kind = problemKindRootRendering;
    }
    // An engine:// document is not the project's to edit: the fields say
    // which pass, and the problem names no document.
    if (auto spelled = graphDocumentReference(document); spelled && !spelled->starts_with("engine://")) {
        refused.pass_document = std::move(*spelled);
        refused.pass_document_kind = std::string{kind};
        return refused;
    }
    refused.pass_node.reset();
    return refused;
}

std::string materialPassContractUnmetMessage(
    const ProjectMaterialAssetContainer::RefusedDocument &refused) {
    const auto material =
        refused.material.empty() ? std::string{"a material"} : "material '" + refused.material + "'";
    std::string passes;
    for (std::size_t index = 0; index < refused.passes.size(); ++index)
        passes += std::string{index == 0 ? "" : index + 1 == refused.passes.size() ? " and " : ", "} + "'" +
                  refused.passes[index] + "'";
    const bool several = refused.passes.size() > 1;
    const auto pass = refused.passes.empty() ? std::string{"the pass that draws it"}
                                             : std::string{several ? "passes " : "pass "} + passes;
    const std::string text = std::string{materialPassContractUnmetCode} + ": ";
    if (refused.requirement == "ray_query")
        return text + material + " queries the scene acceleration structure, but " + pass +
               (several ? " do" : " does") + " not declare ray_query: true; declare it on the " +
               (several ? "passes" : "pass") + ", or remove the query from the material's surface";
    return text + material + " needs " + refused.requirement + " of " + pass + ", which " +
           (several ? "they do" : "it does") + " not declare: " + refused.reason;
}

nlohmann::json materialPassContractUnmetFields(
    const std::string &reference,
    const ProjectMaterialAssetContainer::RefusedDocument &refused) {
    nlohmann::json fields{
        {"material", refused.material.empty() ? nlohmann::json(nullptr) : nlohmann::json(refused.material)},
        {"material_document", reference},
        {"pass", refused.passes.empty() ? nlohmann::json(nullptr) : nlohmann::json(refused.passes.front())},
        {"requirement", refused.requirement},
        {"reason", refused.reason},
    };
    if (refused.passes.size() > 1) fields["passes"] = refused.passes;
    return fields;
}

nlohmann::json materialPassContractUnmetProblem(
    const std::string &reference,
    const ProjectMaterialAssetContainer::RefusedDocument &refused) {
    auto problem = materialPassContractUnmetFields(reference, refused);
    problem["code"] = refused.code;
    problem["message"] = materialPassContractUnmetMessage(refused);
    if (refused.pass_document) {
        problem["document"] = *refused.pass_document;
        if (refused.pass_document_kind) problem["document_kind"] = *refused.pass_document_kind;
    }
    if (refused.pass_node) problem["node"] = *refused.pass_node;
    return problem;
}

void ProjectMaterialAssetContainer::Impl::publishProblems() const {
    std::vector<nlohmann::json> problems;
    for (const auto &[reference, document] : refused) problems.push_back(documentProblem(reference, document));
    for (const auto &[reference, document] : reload_failures)
        problems.push_back(documentProblem(reference, document, true));
    // EB-6: the scene objects whose own material nothing registered holds.
    for (const auto &object : unresolved_objects) {
        nlohmann::json fields{{"object", object.object},
                              {"scene_id", object.scene_id},
                              {"material", object.material},
                              {"model", object.model}};
        if (object.name) fields["name"] = *object.name;
        if (object.primitive)
            fields["primitive"] = {{"mesh", object.primitive->first}, {"primitive", object.primitive->second}};
        const auto who = object.name ? "'" + *object.name + "'" : "#" + std::to_string(object.object);
        nlohmann::json problem{
            {"severity", std::string{problemSeverityWarning}},
            {"source", std::string{problemSourceStartup}},
            {"code", "material.object_material_unresolved@1"},
            {"message", "material.object_material_unresolved@1: scene object " + who + " sets its own material '" +
                            object.material + "', which neither model '" + object.model +
                            "' nor a registered material document holds; it draws with the model's material"},
            {"document_kind", std::string{problemKindScene}},
            {"fields", std::move(fields)},
        };
        if (scene_document) problem["document"] = *scene_document;
        problems.push_back(std::move(problem));
    }
    for (const auto &[model, materials] : unresolved_bindings) {
        std::string names;
        for (const auto &material : materials) names += (names.empty() ? "'" : ", '") + material + "'";
        problems.push_back({
            {"severity", std::string{problemSeverityWarning}},
            {"source", std::string{problemSourceStartup}},
            {"code", "material.binding_unresolved@1"},
            {"message", "material.binding_unresolved@1: model '" + model + "' binds " + names +
                            ", which no registered material document holds; its primitives draw with the "
                            "model's own material"},
            {"document_kind", std::string{problemKindMaterial}},
            {"fields", {{"model", model}, {"materials", materials}}},
        });
    }
    auto *list = FastModuleContainer::tryGet<ProblemList>();
    if (list == nullptr) {
        if (problems.empty()) return;
        try {
            list = &GET_MODULE(ProblemList);
        } catch (const std::exception &) {
            return;
        }
    }
    list->replace(materialProblemDomain, std::move(problems));
}

std::vector<std::string>
ProjectMaterialAssetContainer::Impl::registerDocuments(
    std::span<const std::string> references, bool replace,
    std::vector<GlobalMaterialId> *previous) {
    const auto replaced = [&](const std::string &document) {
        return replace && std::find(references.begin(), references.end(), document) != references.end();
    };
    auto &paths = GET_MODULE(PathResolver);
    std::vector<SourceDocument> documents;
    documents.reserve(references.size());
    MaterialSurfaceCatalog surfaces;
    for (const auto &reference : references) {
        const auto parsed_reference =
            parsePathReference(reference);
        if (parsed_reference.fragment) {
            throw std::runtime_error(
                "project material document must not "
                "contain a fragment: " +
                reference);
        }
        auto document = SourceDocument{
            .reference = reference,
            .path =
                paths.resolveExistingFile(
                    reference),
            .key =
                watch::makeAssetKey(
                    reference),
        };
        document.json = readJsonFile(
            document.path,
            "project material document");
        for (const auto &surface_reference :
             referencedSurfaces(document.json)) {
            if (surfaces.contains(surface_reference)) {
                continue;
            }
            try {
                surfaces.emplace(
                    surface_reference,
                    parseSurfaceFormat(
                        paths.loadText(surface_reference),
                        surface_reference));
            } catch (const std::exception &error) {
                throw std::runtime_error(
                    "project material document '" +
                    reference +
                    "' surface '" + surface_reference +
                    "': " + error.what());
            }
        }
        documents.push_back(std::move(document));
    }

    std::map<std::string, std::string>
        material_sources = material_documents;
    std::erase_if(material_sources, [&](const auto &entry) { return replaced(entry.second); });
    std::vector<PreparedProjectMaterial> prepared;
    for (std::size_t document_index = 0;
         document_index < documents.size();
         ++document_index) {
        auto &document = documents[document_index];
        try {
            document.parsed =
                parseMaterialFormatJson(
                    document.json, surfaces);
        } catch (const std::exception &error) {
            throw std::runtime_error(
                "project material document '" +
                document.reference +
                "': " + error.what());
        }
        for (const auto &warning :
             document.parsed.warnings) {
            LOG_WARNING(
                logger,
                "project material document '{}': {}",
                document.reference, warning);
        }
        for (auto &definition :
             document.parsed.materials) {
            const auto [existing, inserted] =
                material_sources.emplace(
                    definition.name,
                    document.reference);
            if (!inserted) {
                throw std::runtime_error(
                    "duplicate project material name '" +
                    definition.name + "' in '" +
                    existing->second + "' and '" +
                    document.reference + "'");
            }
            if (!definition.surface) {
                throw std::runtime_error(
                    "project material '" +
                    definition.name + "' in '" +
                    document.reference +
                    "' requires a surface for runtime "
                    "lowering");
            }
            const auto found =
                surfaces.find(*definition.surface);
            if (found == surfaces.end()) {
                throw std::runtime_error(
                    "project material '" +
                    definition.name +
                    "' surface '" +
                    *definition.surface +
                    "' was not loaded");
            }
            auto lowered =
                lowerMaterial(
                    definition, found->second,
                    projectMaterialLoweringCapabilities());
            auto variants =
                lowerMaterialVariants(
                    definition, surfaces,
                    projectMaterialLoweringCapabilities());
            prepared.push_back({
                .document_index = document_index,
                .definition =
                    std::move(definition),
                .lowered = std::move(lowered),
                .variants =
                    std::move(variants),
            });
        }
    }

    auto &materials = GET_MODULE(MaterialContainer);
    auto &standard =
        GET_MODULE(StandardMaterialResource);
    auto &shaders = GET_MODULE(ShaderLibrary);
    if (!texture_resolver) {
        texture_resolver =
            std::make_unique<
                ProjectMaterialTextureResolver>(
                materials, standard, paths);
    }
    const auto shader_defines =
        activeMaterialShaderDefines();
    std::vector<std::vector<
        MaterialContainer::
            ReloadableMaterialValuesBinding>>
        reload_bindings(documents.size());
    std::vector<ModelTemplate::NamedMaterial> fresh;
    fresh.reserve(prepared.size());
    std::vector<std::string> registered;
    registered.reserve(prepared.size());
    // A registration that throws part way takes back what it registered
    // (MT-3: the registration it was to replace stays as it was).
    std::vector<GlobalMaterialId> fresh_ids;
    try {
        for (auto &entry : prepared) {
            const auto &surface_reference =
                *entry.definition.surface;
            const auto &surface =
                surfaces.at(surface_reference);
            auto info = makeMaterialInfo(
                entry.definition.base,
                entry.lowered, surface,
                surface_reference, shader_defines,
                standard, shaders,
                *texture_resolver);
            const auto &source_document =
                documents.at(entry.document_index);
            info.name = entry.definition.name;
            info.source_asset = source_document.reference;
            info.source_document = source_document.reference;
            const auto material =
                materials.registerMaterial(
                    std::move(info));
            fresh_ids.push_back(material);

            std::vector<MaterialContainer::
                            NamedMaterialVariantRegistration>
                variants;
            variants.reserve(entry.variants.size());
            for (const auto &variant :
                 entry.variants) {
                const auto &variant_surface =
                    surfaces.at(
                        variant.material.surface);
                auto variant_info = makeMaterialInfo(
                    entry.definition.base,
                    variant.material,
                    variant_surface,
                    variant.material.surface,
                    shader_defines,
                    standard, shaders,
                    *texture_resolver);
                variant_info.name =
                    entry.definition.name + "#variant/" +
                    variant.name;
                variant_info.source_asset =
                    source_document.reference;
                variant_info.source_document =
                    source_document.reference;
                variants.push_back({
                    .name = variant.name,
                    .material = std::move(variant_info),
                });
            }
            materials.registerMaterialVariants(
                material, std::move(variants));

            fresh.push_back({
                .name = entry.definition.name,
                .material = material,
                .routing = entry.lowered.routing,
            });
            registered.push_back(entry.definition.name);
            auto &bindings =
                reload_bindings.at(
                    entry.document_index);
            bindings.push_back({
                .name = entry.definition.name,
                .material = material,
            });
            for (const auto &variant :
                 entry.variants) {
                bindings.push_back({
                    .name = entry.definition.name,
                    .material = material,
                    .variant = variant.name,
                });
            }
        }

        for (std::size_t index = 0;
             index < documents.size(); ++index) {
            if (replace)
                materials.forgetReloadableMaterialValuesFile(
                    documents[index].key);
            materials
                .registerReloadableMaterialValuesFile(
                    documents[index].key,
                    documents[index].path,
                    surfaces,
                    reload_bindings[index]);
        }
    } catch (...) {
        ReleaseErrorSink::record(
            "project material registration",
            materials.releaseModelResources(
                std::move(fresh_ids), {}, false));
        throw;
    }

    // The names the replaced documents held leave with their materials.
    for (auto it = named_materials.begin(); it != named_materials.end();) {
        const auto source = material_documents.find(it->name);
        if (source != material_documents.end() && replaced(source->second)) {
            if (previous != nullptr) previous->push_back(it->material);
            material_documents.erase(source);
            it = named_materials.erase(it);
        } else {
            ++it;
        }
    }
    for (std::size_t index = 0; index < fresh.size(); ++index) {
        material_documents.insert_or_assign(
            fresh[index].name,
            documents.at(prepared[index].document_index).reference);
        named_materials.push_back(std::move(fresh[index]));
    }
    ++names_generation;
    return registered;
}

ProjectMaterialAssetContainer::
    ProjectMaterialAssetContainer()
    : impl_{std::make_unique<Impl>()} {
    const auto asset_json = nlohmann::json::parse(
        GET_MODULE(ProjectBasicConfig).assetDataJson());
    const auto asset_data =
        parseAssetDataFormatJson(asset_json);
    if (asset_data.materials.empty()) {
        return;
    }
    std::vector<std::string> references;
    references.reserve(asset_data.materials.size());
    for (const auto &declaration :
         asset_data.materials) {
        references.push_back(declaration.path);
    }
    // FO-1: --strict-assets keeps a document that does not register fatal.
    const auto *launch = FastModuleContainer::tryGet<EngineLaunchConfig>();
    if (launch != nullptr && launch->strict_assets) {
        (void)impl_->registerDocuments(references);
        return;
    }
    try {
        (void)impl_->registerDocuments(references);
        return;
    } catch (const std::exception &) {
        // Taken back whole (registerDocuments registers all or nothing);
        // registered again one document at a time below.
    }
    // FO-1: a document that does not register is left out and listed; the
    // engine starts with the others' materials.
    for (const auto &reference : references) {
        try {
            (void)impl_->registerDocuments(std::span<const std::string>{&reference, 1});
        } catch (const PassContractRefusal &refusal) {
            // FO-2: the document reads; a pass it is drawn by does not declare
            // what its material uses. Listed where the pass is declared.
            auto refused = materialPassContractRefusal(reference, refusal);
            LOG_ERROR(logger,
                      "{}: the material document '{}' did not register; the engine starts without its "
                      "materials (pass --strict-assets to stop instead): {}",
                      refused.code, reference, materialPassContractUnmetMessage(refused));
            if (const auto *passes = FastModuleContainer::tryGet<RenderingPassContainer>())
                refused.generation = passes->materialPassGeneration();
            impl_->refused.insert_or_assign(reference, std::move(refused));
        } catch (const std::exception &error) {
            auto refused = refusedDocumentOf(reference, error.what());
            LOG_ERROR(logger,
                      "{}: the material document '{}' did not register; the engine starts without its "
                      "materials (pass --strict-assets to stop instead): {}",
                      refused.code, reference, refused.reason);
            impl_->refused.insert_or_assign(reference, std::move(refused));
        }
    }
    impl_->publishProblems();
}

ProjectMaterialAssetContainer::
    ~ProjectMaterialAssetContainer() = default;

std::span<const ModelTemplate::NamedMaterial>
ProjectMaterialAssetContainer::namedMaterials()
    const noexcept {
    return impl_->named_materials;
}

GlobalMaterialId
ProjectMaterialAssetContainer::materialByName(
    std::string_view name) const {
    const auto found = std::find_if(
        impl_->named_materials.begin(),
        impl_->named_materials.end(),
        [name](const auto &material) {
            return material.name == name;
        });
    if (found == impl_->named_materials.end()) {
        throw std::out_of_range(
            "unknown project material: " +
            std::string{name});
    }
    return found->material;
}

std::vector<std::string>
ProjectMaterialAssetContainer::registerDocument(
    const std::string &reference) {
    return impl_->registerDocuments(
        std::span<const std::string>{&reference, 1});
}

ProjectMaterialAssetContainer::Reregistered
ProjectMaterialAssetContainer::reregisterDocument(
    const std::string &reference) {
    Reregistered result;
    result.names = impl_->registerDocuments(
        std::span<const std::string>{&reference, 1}, true,
        &result.previous);
    // EB-2: what it reads now is registered; a failed reload of it is over.
    // EB-6: so is a pass's refusal of what it read before.
    const auto failures = impl_->reload_failures.erase(reference);
    if (failures + impl_->refused.erase(reference) != 0) impl_->publishProblems();
    return result;
}

std::vector<GlobalMaterialId>
ProjectMaterialAssetContainer::unregisterDocument(
    const std::string &reference) {
    std::vector<GlobalMaterialId> released;
    auto &named = impl_->named_materials;
    for (auto it = named.begin(); it != named.end();) {
        const auto source =
            impl_->material_documents.find(it->name);
        if (source != impl_->material_documents.end() &&
            source->second == reference) {
            released.push_back(it->material);
            impl_->material_documents.erase(source);
            it = named.erase(it);
        } else {
            ++it;
        }
    }
    if (!released.empty()) {
        GET_MODULE(MaterialContainer)
            .forgetReloadableMaterialValuesFile(
                watch::makeAssetKey(reference));
        ++impl_->names_generation;
    }
    // EB-6: a document refused (at start or over RPC) leaves the list with it.
    const auto failures = impl_->reload_failures.erase(reference);
    if (failures + impl_->refused.erase(reference) != 0) impl_->publishProblems();
    return released;
}

std::optional<std::string>
ProjectMaterialAssetContainer::documentOf(
    std::string_view name) const {
    const auto found =
        impl_->material_documents.find(std::string{name});
    if (found == impl_->material_documents.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<ProjectMaterialAssetContainer::RefusedDocument>
ProjectMaterialAssetContainer::refusedDocument(
    std::string_view reference) const {
    const auto found = impl_->refused.find(reference);
    if (found == impl_->refused.end()) return std::nullopt;
    return found->second;
}

bool ProjectMaterialAssetContainer::hasRefusedDocuments() const noexcept {
    return !impl_->refused.empty();
}

void ProjectMaterialAssetContainer::noteUnresolvedBindings(
    const std::string &model, std::vector<std::string> materials) {
    std::sort(materials.begin(), materials.end());
    materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
    if (materials.empty()) {
        // EB-2: the model's bindings resolve again.
        if (impl_->unresolved_bindings.erase(model) != 0) impl_->publishProblems();
        return;
    }
    auto &noted = impl_->unresolved_bindings[model];
    if (noted == materials) return;
    noted = std::move(materials);
    impl_->publishProblems();
}

std::vector<std::string> ProjectMaterialAssetContainer::unresolvedBindings(std::string_view model) const {
    const auto found = impl_->unresolved_bindings.find(std::string{model});
    return found == impl_->unresolved_bindings.end() ? std::vector<std::string>{} : found->second;
}

std::optional<std::string> ProjectMaterialAssetContainer::Impl::registeredReferenceOf(
    const watch::AssetKey &key) const {
    for (const auto &[name, reference] : material_documents) {
        (void)name;
        if (watch::makeAssetKey(reference) == key) return reference;
    }
    return std::nullopt;
}

std::vector<std::string> ProjectMaterialAssetContainer::refusedReferences() const {
    std::vector<std::string> references;
    for (const auto &[reference, refused] : impl_->refused) {
        (void)refused;
        references.push_back(reference);
    }
    return references;
}

std::optional<std::string> ProjectMaterialAssetContainer::refusedReferenceOf(
    const watch::AssetKey &key) const {
    for (const auto &[reference, refused] : impl_->refused) {
        (void)refused;
        try {
            if (watch::makeAssetKey(reference) == key) return reference;
        } catch (const std::exception &) {
            // A reference the watch cannot name is never a watched file.
        }
    }
    return std::nullopt;
}

ProjectMaterialAssetContainer::Recovered
ProjectMaterialAssetContainer::recoverDocument(const std::string &reference) {
    const auto refused = impl_->refused.find(reference);
    if (refused == impl_->refused.end())
        throw std::logic_error("material document '" + reference + "' was not refused at start");
    // As register_material does: nothing in flight draws while the material
    // container takes the new registration.
    if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
    Recovered result;
    // EB-6: a document registered already (an edit a pass refused) takes its
    // names over; the previous registration is released once nothing draws
    // with it.
    const bool registered =
        std::any_of(impl_->material_documents.begin(), impl_->material_documents.end(),
                    [&](const auto &entry) { return entry.second == reference; });
    std::vector<GlobalMaterialId> previous;
    // Throws, registering nothing, when the document still does not register;
    // its refusal and problem stay as they were.
    result.names = impl_->registerDocuments(std::span<const std::string>{&reference, 1}, registered,
                                            registered ? &previous : nullptr);
    impl_->refused.erase(refused);
    impl_->reload_failures.erase(reference);
    LOG_INFO(logger, "material document '{}' refused before is registered now ({} material(s))", reference,
             result.names.size());

    // The models whose bindings kept their own material for want of one of
    // the names now registered: rebuilt so their primitives draw it.
    const auto named = [&](const std::string &material) {
        return std::find(result.names.begin(), result.names.end(), material) != result.names.end();
    };
    std::vector<std::string> models;
    for (const auto &[model, materials] : impl_->unresolved_bindings)
        if (std::any_of(materials.begin(), materials.end(), named)) models.push_back(model);
    // EB-6: and those drawing the previous registration, and those of the
    // scene objects whose own material it now holds.
    const auto add = [&](const std::string &model) {
        if (std::find(models.begin(), models.end(), model) == models.end()) models.push_back(model);
    };
    if (const auto *container = FastModuleContainer::tryGet<ModelAssetContainer>(); container && !previous.empty())
        for (const auto &model : container->modelsDrawingMaterials(previous)) add(model);
    for (const auto &object : impl_->unresolved_objects)
        if (named(object.material)) add(object.model);
    const auto releasePrevious = [&] {
        if (previous.empty()) return;
        if (auto *materials = FastModuleContainer::tryGet<MaterialContainer>())
            ReleaseErrorSink::record("material registration replaced",
                                     materials->releaseModelResources(std::move(previous), {}, true));
    };
    if (models.empty()) releasePrevious();
    if (!models.empty()) {
        auto *container = FastModuleContainer::tryGet<ModelAssetContainer>();
        auto *reload = FastModuleContainer::tryGet<watch::ReloadService>();
        if (container == nullptr || reload == nullptr) {
            result.restart_reason = "the models binding its materials are rebuilt when the engine starts again "
                                    "(this host cannot rebuild them in place)";
        } else {
            // A rebuild that throws leaves the models as they were: their
            // warnings come back with them.
            const auto noted = impl_->unresolved_bindings;
            try {
                std::vector<watch::AssetKey> keys;
                for (const auto &model : models) {
                    if (!container->hasModel(model)) continue;
                    keys.push_back(container->reloadKey(model));
                    result.rebuilt_models.push_back(model);
                }
                if (!keys.empty()) reload->reloadModelsNow(keys);
                releasePrevious();
            } catch (const std::exception &error) {
                // The previous registration stays: the models still draw it.
                result.rebuilt_models.clear();
                impl_->unresolved_bindings = noted;
                result.restart_reason = "the material document is registered, but the models binding its materials "
                                        "could not be rebuilt in place: " +
                                        std::string{error.what()};
                LOG_WARNING(logger, "material document '{}': {}", reference, *result.restart_reason);
            }
        }
    }
    impl_->publishProblems();
    return result;
}

void ProjectMaterialAssetContainer::retryPassContractRefusals() {
    const auto *passes = FastModuleContainer::tryGet<RenderingPassContainer>();
    if (passes == nullptr) return;
    const auto generation = passes->materialPassGeneration();
    // EB-6: each document by the generation it was last tried against (one
    // refused over RPC may be newer than one refused at start).
    std::vector<std::string> references;
    for (const auto &[reference, refused] : impl_->refused)
        if (refused.status == "unusable" && refused.generation != generation) references.push_back(reference);
    if (references.empty()) return;
    bool changed = false;
    for (const auto &reference : references) {
        const auto source = impl_->refused.at(reference).source;
        try {
            // Takes the problem off and rebuilds the models that kept their
            // own material for want of it.
            (void)recoverDocument(reference);
        } catch (const PassContractRefusal &refusal) {
            auto refused = materialPassContractRefusal(reference, refusal);
            LOG_INFO(logger, "material document '{}' still does not register: {}", reference,
                     materialPassContractUnmetMessage(refused));
            refused.source = source;
            refused.generation = generation;
            impl_->refused.insert_or_assign(reference, std::move(refused));
            changed = true;
        } catch (const std::exception &error) {
            LOG_WARNING(logger, "material document '{}' still does not register: {}", reference, error.what());
            auto refused = refusedDocumentOf(reference, error.what());
            refused.source = source;
            refused.generation = generation;
            impl_->refused.insert_or_assign(reference, std::move(refused));
            changed = true;
        }
    }
    if (changed) impl_->publishProblems();
}

ProjectMaterialAssetContainer::RefusedDocument
ProjectMaterialAssetContainer::notePassContractRefusal(const std::string &reference,
                                                       const PassContractRefusal &refusal) {
    auto refused = materialPassContractRefusal(reference, refusal);
    refused.source = std::string{problemSourceEditRpc};
    if (const auto *passes = FastModuleContainer::tryGet<RenderingPassContainer>())
        refused.generation = passes->materialPassGeneration();
    LOG_WARNING(logger, "{}: the material document '{}' is written but not drawn until its pass declares what it "
                        "uses: {}",
                refused.code, reference, materialPassContractUnmetMessage(refused));
    impl_->refused.insert_or_assign(reference, refused);
    impl_->publishProblems();
    return refused;
}

namespace {

// EB-6: the scene document project.json names, spelled project://<path>.
std::optional<std::string> sceneDocumentReference() {
    const auto *source = FastModuleContainer::tryGet<ProjectSource>();
    if (source == nullptr || !source->hasProjectSource()) return std::nullopt;
    try {
        const auto project = nlohmann::json::parse(source->loadProjectSource());
        return graphDocumentReference(project.at("basic_config").at("scene_data_json").get<std::string>());
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

} // namespace

void ProjectMaterialAssetContainer::syncObjectMaterialProblems() {
    const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>();
    auto *models = FastModuleContainer::tryGet<ModelAssetContainer>();
    // Only once a scene is loaded: the projection is not read (and so not
    // made) for this.
    if (config == nullptr || models == nullptr || config->runtimeSceneEpoch() == 0) return;
    const SceneProjectionState *state = nullptr;
    try {
        state = &config->sceneProjectionState();
    } catch (const std::exception &) {
        // No scene the engine could read: nothing to look at.
        return;
    }
    auto identity = state->snapshotIdentity();
    if (identity == impl_->objects_scene && impl_->objects_names_generation == impl_->names_generation) return;
    impl_->objects_scene = std::move(identity);
    impl_->objects_names_generation = impl_->names_generation;

    const auto registered = [&](std::string_view name) {
        return std::any_of(impl_->named_materials.begin(), impl_->named_materials.end(),
                           [&](const auto &named) { return named.name == name; });
    };
    std::vector<Impl::UnresolvedObjectMaterial> found;
    for (const auto &scene : state->resolved().scenes()) {
        for (const auto &object : scene.objects) {
            for (const auto &component : object.components) {
                if (component.name != "simplemodelview" || !component.effective_json.is_object()) continue;
                const auto &view = component.effective_json;
                const auto list = view.find("materials");
                if (list == view.end() || !list->is_object() || !list->contains("rows") ||
                    !list->at("rows").is_array())
                    continue;
                const auto model = view.value("model", std::string{});
                if (model.empty() || !models->hasModel(model)) continue;
                const auto &glTF = models->getModelTemplateByName(model).named_materials;
                for (const auto &row : list->at("rows")) {
                    if (!row.is_object() || !row.contains("material") || !row.at("material").is_string()) continue;
                    const auto material = row.at("material").get<std::string>();
                    if (registered(material) ||
                        std::any_of(glTF.begin(), glTF.end(), [&](const auto &named) { return named.name == material; }))
                        continue;
                    Impl::UnresolvedObjectMaterial entry{
                        .object = object.authoring_object_id.value,
                        .name = object.name,
                        .scene_id = scene.scene_id,
                        .material = material,
                        .model = model,
                    };
                    if (row.contains("mesh") && row.at("mesh").is_number_unsigned() && row.contains("primitive") &&
                        row.at("primitive").is_number_unsigned())
                        entry.primitive = std::pair{row.at("mesh").get<std::uint32_t>(),
                                                    row.at("primitive").get<std::uint32_t>()};
                    found.push_back(std::move(entry));
                }
            }
        }
    }
    if (!found.empty() && !impl_->scene_document) impl_->scene_document = sceneDocumentReference();
    if (found == impl_->unresolved_objects) return;
    impl_->unresolved_objects = std::move(found);
    impl_->publishProblems();
}

void ProjectMaterialAssetContainer::noteReloadFailure(const watch::AssetKey &key, const std::string &error) {
    const auto reference = impl_->registeredReferenceOf(key);
    if (!reference) return;
    impl_->reload_failures.insert_or_assign(*reference, refusedDocumentOf(*reference, error));
    impl_->publishProblems();
}

void ProjectMaterialAssetContainer::clearReloadFailure(const watch::AssetKey &key) noexcept {
    try {
        const auto reference = impl_->registeredReferenceOf(key);
        if (!reference || impl_->reload_failures.erase(*reference) == 0) return;
        impl_->publishProblems();
    } catch (const std::exception &error) {
        LOG_WARNING(logger, "material problem list not updated: {}", error.what());
    }
}

} // namespace Pelican
