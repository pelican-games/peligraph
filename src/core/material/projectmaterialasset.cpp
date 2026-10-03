#include "projectmaterialasset.hpp"

#include "../../project/assetdataformat.hpp"
#include "../../project/materialformat.hpp"
#include "../../project/materiallowering.hpp"
#include "../../project/surfaceformat.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../watch/assetkey.hpp"
#include "materialcontainer.hpp"
#include "material.hpp"
#include "materialshaderconfig.hpp"
#include "projectmaterialtextureresolver.hpp"
#include "standardmaterialresource.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    MaterialInfo info{
        .vert_shader = bundles.vertex,
        .frag_shader = bundles.fragment,
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
    std::vector<std::string> registerDocuments(
        std::span<const std::string> references);
};

std::vector<std::string>
ProjectMaterialAssetContainer::Impl::registerDocuments(
    std::span<const std::string> references) {
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
    named_materials.reserve(named_materials.size() +
                            prepared.size());
    std::vector<std::string> registered;
    registered.reserve(prepared.size());

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

        named_materials.push_back({
            .name = entry.definition.name,
            .material = material,
            .routing = entry.lowered.routing,
        });
        material_documents.insert_or_assign(
            entry.definition.name,
            source_document.reference);
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
        materials
            .registerReloadableMaterialValuesFile(
                documents[index].key,
                documents[index].path,
                surfaces,
                reload_bindings[index]);
    }
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
    (void)impl_->registerDocuments(references);
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

} // namespace Pelican
