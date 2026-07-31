#include "modeltemplate.hpp"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {
namespace {

std::uint64_t primitiveKey(std::uint32_t mesh, std::uint32_t primitive) {
    return (static_cast<std::uint64_t>(mesh) << 32u) | primitive;
}

std::string primitiveText(std::uint32_t mesh, std::uint32_t primitive) {
    return "GLB mesh " + std::to_string(mesh) + " primitive " +
           std::to_string(primitive);
}

const ModelTemplate::NamedMaterial *findUniqueNamedMaterial(
    std::span<const ModelTemplate::NamedMaterial> materials,
    std::string_view name, std::string_view domain,
    std::string_view context) {
    const ModelTemplate::NamedMaterial *result = nullptr;
    for (const auto &material : materials) {
        if (material.name != name) continue;
        if (result != nullptr) {
            throw std::runtime_error(
                std::string{context} + " " +
                std::string{domain} + " material name '" +
                std::string{name} + "' is duplicated");
        }
        result = &material;
    }
    return result;
}

std::string routingName(
    const MaterialVariantRouting &routing) {
    return std::string{materialVariantName(routing)};
}

} // namespace

void validateNamedMaterialResolutionDomains(
    const ModelTemplate &model,
    std::span<const ModelTemplate::NamedMaterial>
        project_materials,
    std::string_view model_name) {
    if (project_materials.empty()) return;
    const auto context =
        "material resolution for model '" +
        std::string{model_name} + "'";
    std::unordered_set<std::string> project_names;
    for (const auto &material : project_materials) {
        if (!project_names.insert(material.name).second) {
            throw std::runtime_error(
                context + " project material name '" +
                material.name + "' is duplicated");
        }
    }
    for (const auto &material : model.named_materials) {
        if (project_names.contains(material.name)) {
            throw std::runtime_error(
                context + " material name '" +
                material.name +
                "' is ambiguous between glTF and project "
                "material registries");
        }
    }
}

void applyPrimitiveMaterialBindings(
    ModelTemplate &model, const PrimitiveMaterialBindingDocument &document,
    std::string_view model_name, std::optional<std::string_view> fragment,
    std::span<const ModelTemplate::NamedMaterial>
        project_materials) {
    const auto context = "material bindings for model '" + std::string{model_name} + "'";
    validateNamedMaterialResolutionDomains(
        model, project_materials, model_name);
    if (fragment) {
        throw std::runtime_error(context + " cannot be applied to fragment '" +
                                 std::string{*fragment} +
                                 "'; primitive bindings require a whole-model load");
    }
    if (document.bindings.empty()) {
        throw std::runtime_error(context + " has no binding entries");
    }

    std::unordered_map<std::uint64_t, const ModelPrimitiveRefInfo *> available;
    for (const auto &group : model.material_primitives) {
        for (const auto &primitive : group.primitives) {
            const auto key = primitiveKey(primitive.mesh_index, primitive.primitive_index);
            // A GLB mesh may be instanced by several nodes. One stable
            // mesh/primitive binding intentionally applies to every occurrence.
            available.try_emplace(key, &primitive);
        }
    }

    std::unordered_set<std::string> usd_paths;
    std::unordered_map<std::uint64_t, GlobalMaterialId> resolved;
    for (const auto &binding : document.bindings) {
        if (!usd_paths.insert(binding.usd_path).second) {
            throw std::runtime_error(context + " has duplicate USD path '" +
                                     binding.usd_path + "'");
        }
        const auto key = primitiveKey(binding.mesh_index, binding.primitive_index);
        if (!available.contains(key)) {
            throw std::runtime_error(context + " USD path '" + binding.usd_path +
                                     "' targets missing " +
                                     primitiveText(binding.mesh_index,
                                                   binding.primitive_index));
        }
        if (resolved.contains(key)) {
            throw std::runtime_error(context + " has a collision at " +
                                     primitiveText(binding.mesh_index,
                                                   binding.primitive_index));
        }
        const auto *gltf_material =
            findUniqueNamedMaterial(
                model.named_materials,
                binding.material, "glTF", context);
        const auto *project_material =
            findUniqueNamedMaterial(
                project_materials,
                binding.material, "project", context);
        if (gltf_material != nullptr &&
            project_material != nullptr) {
            throw std::runtime_error(
                context + " material name '" +
                binding.material +
                "' is ambiguous between glTF and project "
                "material registries");
        }
        const auto *material =
            gltf_material != nullptr
                ? gltf_material
                : project_material;
        if (material == nullptr) {
            throw std::runtime_error(context + " USD path '" + binding.usd_path +
                                     "' references missing material '" +
                                     binding.material + "'");
        }
        if (!material->routing) {
            throw std::runtime_error(
                context + " USD path '" +
                binding.usd_path + "' material '" +
                binding.material +
                "' has no routing metadata");
        }
        if (*material->routing != binding.routing) {
            throw std::runtime_error(
                context + " USD path '" +
                binding.usd_path + "' material '" +
                binding.material + "' routing '" +
                routingName(*material->routing) +
                "' does not match binding routing '" +
                routingName(binding.routing) + "'");
        }
        resolved.emplace(key, material->material);
    }

    for (const auto &[key, primitive] : available) {
        if (!resolved.contains(key)) {
            throw std::runtime_error(context + " is missing a binding for " +
                                     primitiveText(primitive->mesh_index,
                                                   primitive->primitive_index));
        }
    }

    std::vector<ModelTemplate::MaterialPrimitives> regrouped;
    std::unordered_map<std::uint64_t, std::size_t> group_by_material;
    for (const auto &source_group : model.material_primitives) {
        for (const auto &primitive : source_group.primitives) {
            const auto target =
                resolved.at(primitiveKey(primitive.mesh_index, primitive.primitive_index));
            const auto group_key =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(target.value)) << 32u) |
                source_group.source_material_index;
            const auto [found, inserted] =
                group_by_material.emplace(group_key, regrouped.size());
            if (inserted) {
                regrouped.push_back(ModelTemplate::MaterialPrimitives{
                    .material = target,
                    .primitives = {},
                    .source_material_index = source_group.source_material_index,
                });
            }
            regrouped.at(found->second).primitives.push_back(primitive);
        }
    }
    model.material_primitives = std::move(regrouped);
}

} // namespace Pelican
