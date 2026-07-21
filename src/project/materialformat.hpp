#pragma once

#include "renderpipeline.hpp"
#include "surfaceformat.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

struct MaterialValue {
    std::string name;
    SurfaceParamValue value;
};

struct MaterialTextureOverride {
    std::string name;
    std::string reference;
};

enum class MaterialRenderPath {
    automatic,
    deferred,
    forward,
};

enum class MaterialAlphaMode {
    opaque,
    mask,
    blend,
};

// OpenPBR/glTF render-state inputs remain material metadata. v1 deliberately
// does not turn these into arbitrary material-level pipeline overrides: the
// pair selects one of six surface wrappers with a fixed state.
struct MaterialVariantRouting {
    MaterialAlphaMode alpha_mode = MaterialAlphaMode::opaque;
    bool double_sided = false;

    friend bool operator==(const MaterialVariantRouting &,
                           const MaterialVariantRouting &) = default;
};

struct PrimitiveMaterialBinding {
    std::string usd_path;
    std::uint32_t mesh_index = 0;
    std::uint32_t primitive_index = 0;
    std::string material;
    MaterialVariantRouting routing;
};

struct PrimitiveMaterialBindingDocument {
    std::string model;
    std::vector<PrimitiveMaterialBinding> bindings;
};

struct MaterialBase {
    std::array<double, 4> base_color_factor{1.0, 1.0, 1.0, 1.0};
    std::optional<std::string> base_color_texture;
    double metallic_factor = 1.0;
    double roughness_factor = 1.0;
    std::optional<std::string> metallic_roughness_texture;
    std::optional<std::string> normal_texture;
    std::optional<std::string> occlusion_texture;
    std::array<double, 3> emissive_factor{0.0, 0.0, 0.0};
    std::optional<std::string> emissive_texture;
};

struct MaterialDefinition {
    std::string name;
    MaterialBase base;
    std::optional<std::string> shader;
    std::vector<std::string> defines;
    std::optional<std::string> surface;
    std::vector<MaterialValue> values;
    std::vector<MaterialTextureOverride> texture_overrides;
    std::optional<MaterialVariantRouting> routing;
    MaterialRenderPath render_path = MaterialRenderPath::automatic;
    std::optional<std::string> exact_pass;
};

struct MaterialFormatDocument {
    std::vector<MaterialDefinition> materials;
    std::vector<std::string> warnings;
};

using MaterialSurfaceCatalog = std::unordered_map<std::string, SurfaceFormatDocument>;

MaterialFormatDocument parseMaterialFormatJson(const nlohmann::json &document);
MaterialFormatDocument parseMaterialFormatJson(const nlohmann::json &document,
                                               const MaterialSurfaceCatalog &surfaces);

PrimitiveMaterialBindingDocument
parsePrimitiveMaterialBindingJson(const nlohmann::json &document);

std::string_view materialAlphaModeName(MaterialAlphaMode mode);
std::string_view materialRenderPathName(MaterialRenderPath path);
std::string_view materialVariantName(const MaterialVariantRouting &routing);
SurfaceRenderState materialVariantRenderState(const MaterialVariantRouting &routing);
bool materialVariantKeepsFace(const MaterialVariantRouting &routing,
                              bool front_facing);
bool materialMaskKeepsFragment(double alpha, double alpha_cutoff);

std::string dumpPrimitiveMaterialBindings(
    const PrimitiveMaterialBindingDocument &document);

} // namespace Pelican
