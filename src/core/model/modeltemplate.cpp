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

} // namespace

void applyPrimitiveMaterialBindings(
    ModelTemplate &model, const PrimitiveMaterialBindingDocument &document,
    std::string_view model_name, std::optional<std::string_view> fragment,
    PrimitiveMaterialResolver resolve_material) {
    const auto context = "material bindings for model '" + std::string{model_name} + "'";
    if (fragment) {
        throw std::runtime_error(context + " cannot be applied to fragment '" +
                                 std::string{*fragment} +
                                 "'; primitive bindings require a whole-model load");
    }
    if (document.bindings.empty()) {
        throw std::runtime_error(context + " has no binding entries");
    }

    if (!resolve_material) {
        resolve_material = [&model, &context](const PrimitiveMaterialBinding &binding)
            -> std::optional<GlobalMaterialId> {
            std::optional<GlobalMaterialId> result;
            for (const auto &named : model.named_materials) {
                if (named.name != binding.material) continue;
                if (result) {
                    throw std::runtime_error(context + " material name '" +
                                             binding.material + "' is duplicated");
                }
                result = named.material;
            }
            return result;
        };
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
        auto material = resolve_material(binding);
        if (!material) {
            throw std::runtime_error(context + " USD path '" + binding.usd_path +
                                     "' references missing material '" +
                                     binding.material + "'");
        }
        resolved.emplace(key, *material);
    }

    for (const auto &[key, primitive] : available) {
        if (!resolved.contains(key)) {
            throw std::runtime_error(context + " is missing a binding for " +
                                     primitiveText(primitive->mesh_index,
                                                   primitive->primitive_index));
        }
    }

    std::vector<ModelTemplate::MaterialPrimitives> regrouped;
    std::unordered_map<int, std::size_t> group_by_material;
    for (const auto &source_group : model.material_primitives) {
        for (const auto &primitive : source_group.primitives) {
            const auto target =
                resolved.at(primitiveKey(primitive.mesh_index, primitive.primitive_index));
            const auto [found, inserted] =
                group_by_material.emplace(target.value, regrouped.size());
            if (inserted) {
                regrouped.push_back(ModelTemplate::MaterialPrimitives{
                    .material = target,
                    .primitives = {},
                });
            }
            regrouped.at(found->second).primitives.push_back(primitive);
        }
    }
    model.material_primitives = std::move(regrouped);
}

} // namespace Pelican
