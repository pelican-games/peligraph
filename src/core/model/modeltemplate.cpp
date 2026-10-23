#include "modeltemplate.hpp"
#include "../../project/projectpathresolver.hpp"
#include "../log.hpp"
#include <algorithm>

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {

bool ModelTemplate::hasPrimitive(std::uint32_t mesh, std::uint32_t primitive) const noexcept {
    for (const auto &group : material_primitives)
        for (const auto &part : group.primitives)
            if (part.mesh_index == mesh && part.primitive_index == primitive) return true;
    return false;
}
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

// Engine primitives also pass through glTF and receive material indices, but
// only materials from a file-backed model use an asset fragment reference.
bool hasFileMaterialReference(const ModelTemplate &model, const ModelTemplate::NamedMaterial &material) {
    return material.index && !model.authored_asset_reference.empty() &&
           !model.authored_asset_reference.starts_with("engine://");
}

} // namespace

std::string modelMaterialReference(const ModelTemplate &model, const ModelTemplate::NamedMaterial &material) {
    if (!hasFileMaterialReference(model, material)) return material.name;
    return parsePathReference(model.authored_asset_reference).path + "#material/" + material.name +
           "@" + std::to_string(*material.index);
}

std::string modelMaterialDisplayName(const ModelTemplate &model, const ModelTemplate::NamedMaterial &material) {
    const auto duplicates = std::count_if(model.named_materials.begin(), model.named_materials.end(),
        [&](const auto &other) { return other.name == material.name; });
    return material.name + (duplicates > 1 && hasFileMaterialReference(model, material) ? " (" + std::to_string(*material.index) + ")" : "");
}

ModelMaterialResolution resolveModelMaterial(const ModelTemplate &model, std::string_view reference,
    std::span<const ModelTemplate::NamedMaterial> project_materials) {
    const bool explicit_gltf = reference.find("#material/") != std::string_view::npos;
    if (!explicit_gltf) {
        if (const auto *project = findUniqueNamedMaterial(project_materials, reference, "project", "material resolution"))
            return {project, std::string{reference}};
    }
    AssetFragmentRef requested{.kind = "material", .path = std::string{reference}};
    if (explicit_gltf) {
        auto parsed = parsePathReference(reference);
        const auto normalize = [](std::string path) {
            if (path.starts_with("project://")) path.erase(0, 10);
            return std::filesystem::path(path).lexically_normal().generic_string();
        };
        if (normalize(parsed.path) != normalize(parsePathReference(model.authored_asset_reference).path))
            throw std::runtime_error("material reference must name the model's own asset file");
        requested = *parsed.fragment;
    }
    std::vector<AssetFragmentCandidate> candidates;
    for (std::size_t i = 0; i < model.named_materials.size(); ++i) {
        const auto &material = model.named_materials[i];
        candidates.push_back({material.index.value_or(static_cast<int>(i)), material.name, material.name, {material.name}});
    }
    if (!explicit_gltf && std::none_of(candidates.begin(), candidates.end(), [&](const auto &c) { return c.name == reference; }))
        return {};
    const auto selected = resolveAssetFragment(requested, candidates);
    const auto &material = model.named_materials[selected.candidate];
    if (selected.name_mismatch && logger)
        LOG_WARNING(logger, "stale material reference '{}': current name is '{}' (index wins)", reference, material.name);
    else if (!explicit_gltf && hasFileMaterialReference(model, material) && logger)
        LOG_INFO(logger, "legacy material '{}' resolved as '{}'", reference, modelMaterialReference(model, material));
    return {&material, modelMaterialReference(model, material), selected.name_mismatch, requested.path};
}

void applyPrimitiveMaterialBindings(
    ModelTemplate &model, const PrimitiveMaterialBindingDocument &document,
    std::string_view model_name, std::optional<std::string_view> fragment,
    std::span<const ModelTemplate::NamedMaterial>
        project_materials,
    const std::function<bool(const PrimitiveMaterialBinding &)>
        &keep_own_material) {
    const auto context = "material bindings for model '" + std::string{model_name} + "'";
    if (fragment) {
        throw std::runtime_error(context + " cannot be applied to fragment '" +
                                 std::string{*fragment} +
                                 "'; primitive bindings require a whole-model load");
    }
    if (document.bindings.empty()) {
        throw std::runtime_error(context + " has no binding entries");
    }

    std::unordered_map<std::uint64_t, const ModelPrimitiveRefInfo *> available;
    // FO-1: the material each primitive draws with before the bindings.
    std::unordered_map<std::uint64_t, GlobalMaterialId> own_material;
    for (const auto &group : model.material_primitives) {
        for (const auto &primitive : group.primitives) {
            const auto key = primitiveKey(primitive.mesh_index, primitive.primitive_index);
            // A GLB mesh may be instanced by several nodes. One stable
            // mesh/primitive binding intentionally applies to every occurrence.
            available.try_emplace(key, &primitive);
            own_material.try_emplace(key, group.material);
        }
    }

    std::vector<MaterialBindingIssue> issues;
    std::unordered_set<std::string> usd_paths;
    std::unordered_map<std::uint64_t, GlobalMaterialId> resolved;
    for (std::size_t row = 0; row < document.bindings.size(); ++row) {
        const auto &binding = document.bindings[row];
        if (!usd_paths.insert(binding.usd_path).second) {
            throw std::runtime_error(context + " has duplicate USD path '" +
                                     binding.usd_path + "'");
        }
        const auto key = primitiveKey(binding.mesh_index, binding.primitive_index);
        if (!available.contains(key)) {
            issues.push_back({row, binding.mesh_index, binding.primitive_index,
                              binding.usd_path, {}, "targets missing " +
                              primitiveText(binding.mesh_index, binding.primitive_index)});
            continue;
        }
        if (resolved.contains(key)) {
            throw std::runtime_error(context + " has a collision at " +
                                     primitiveText(binding.mesh_index,
                                                   binding.primitive_index));
        }
        ModelMaterialResolution selection;
        std::string resolution_error;
        try { selection = resolveModelMaterial(model, binding.material, project_materials); }
        catch (const std::exception &error) { resolution_error = error.what(); }
        const auto *material = selection.material;
        if (selection.name_mismatch)
            issues.push_back({row, binding.mesh_index, binding.primitive_index, binding.usd_path,
                              binding.material, "stale material reference", true, selection.written_name, material->name});
        if (material == nullptr) {
            if (keep_own_material) (void)keep_own_material(binding);
            issues.push_back({row, binding.mesh_index, binding.primitive_index,
                              binding.usd_path, binding.material,
                              resolution_error.empty() ? "references missing material '" + binding.material + "'" : resolution_error});
            resolved.emplace(key, own_material.at(key));
            continue;
        }
        if (available.at(key)->vat_deformed && !material->vat_playback) {
            throw std::runtime_error(context + " " +
                primitiveText(binding.mesh_index, binding.primitive_index) +
                " material '" + binding.material + "': VAT primitives keep their baked material");
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
            issues.push_back({std::nullopt, primitive->mesh_index, primitive->primitive_index,
                              {}, {}, "is missing a binding for " +
                              primitiveText(primitive->mesh_index, primitive->primitive_index)});
            resolved.emplace(key, own_material.at(key));
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
                source_group.source_material_index.value_or(noSourceMaterialIndex);
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
    model.binding_issues = std::move(issues);
}

} // namespace Pelican
