#pragma once
// RC-7e: two runtime variants (flat, XR) of one graph. R0 no graph uses; R1
// only the flat variant uses (written by the project feature named "taa",
// which XR excludes); H is that feature's history target no pass uses (the
// XR variant does not declare it). A config holds one graph: every graph
// gets its own output_transform pass, and pass names are unique across the
// config (compileSurfaceResourceContracts).
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace Pelican::TestSupport {

struct Rc7eShaders {
    std::string vertex = "engine://fullscreen";
    std::string fill_red = "engine://passthrough";
    std::string fill_green = "engine://passthrough";
    std::string copy = "engine://passthrough";
};

struct Rc7eOptions {
    // The graph fills and presents R0 instead of A (A then is the unused one).
    bool g1_uses_r0 = false;
    // false: R0 and H are not declared at all (the reference generation).
    bool declare_unused = true;
    nlohmann::json history_usage = nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"});
    // Declares S, R8G8B8A8_SRGB with STORAGE and SAMPLED, no graph uses.
    bool declare_srgb_storage = false;
    // Adds a project feature named "cube_capture" (the standard cube capture
    // provider's feature): one pass of the $capture/cube family writes
    // cube_face; cube_capture_color, which the provider looks up by name, no
    // node uses.
    bool cube_capture = false;
};

// Writes <root>/features/rc7e_taa.json and returns the authored rendering
// section (renderGraphRoot makes the graph root of it).
inline nlohmann::json rc7eRenderingSection(const std::filesystem::path &root, const Rc7eShaders &shaders = {},
                                           const Rc7eOptions &options = {}) {
    using J = nlohmann::json;
    const auto target = [](const char *name) {
        return J{{"name", name}, {"format", "R8G8B8A8_UNORM"}, {"format_class", "data"},
                 {"extent_scale", 1.0}, {"usage", J::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
    };
    const auto fill = [&](const std::string &name, const char *out, const std::string &fragment) {
        return J{{"name", name}, {"type", "fullscreen"}, {"output", {{"color", out}, {"depth", nullptr}}},
                 {"shader", {{"vertex", shaders.vertex}, {"fragment", fragment}}}};
    };
    J feature{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "taa"},
              {"render_targets", J::array()},
              {"passes", J::array({J{{"insert", "after:g1_fill"}, {"pass", fill("rc7e_fill_r1", "R1", shaders.fill_green)}}})}};
    if (options.declare_unused)
        feature["render_targets"].push_back({{"name", "H"}, {"format", "R8G8B8A8_UNORM"}, {"format_class", "data"},
                                             {"extent_scale", 1.0}, {"history", true}, {"usage", options.history_usage}});
    std::filesystem::create_directories(root / "features");
    std::ofstream{root / "features" / "rc7e_taa.json", std::ios::binary | std::ios::trunc} << feature.dump(2);
    const char *used = options.g1_uses_r0 ? "R0" : "A";
    auto targets = J::array({target("A"), target("R1")});
    if (options.declare_unused || options.g1_uses_r0) targets.push_back(target("R0"));
    if (options.declare_srgb_storage) {
        auto srgb = target("S");
        srgb["format"] = "R8G8B8A8_SRGB";
        srgb["usage"] = J::array({"STORAGE", "SAMPLED"});
        targets.push_back(std::move(srgb));
    }
    auto features = J::array({"project://features/rc7e_taa.json"});
    if (options.cube_capture) {
        const auto cube_target = [](const char *name, const char *format, const char *format_class) {
            return J{{"name", name}, {"extent_scale", 1.0}, {"width", 16}, {"height", 16}, {"layers", 6},
                     {"format", format}, {"format_class", format_class},
                     {"usage", J::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
        };
        auto color = cube_target("cube_capture_color", "R16G16B16A16_SFLOAT", "explicit(R16G16B16A16_SFLOAT)");
        color["dimension"] = "cube";
        color["mip_levels"] = 1;
        auto face = fill("rc7e_cube_face", "cube_face", shaders.fill_green);
        face["resolution_domain"] = "independent";
        face["view_family"] = "$capture/cube";
        face["output"]["color"] = J::array({"cube_face"});
        const J cube{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "cube_capture"},
                     {"render_targets", J::array({std::move(color), cube_target("cube_face", "R8_UNORM", "data")})},
                     {"passes", J::array({J{{"insert", "after:g1_fill"}, {"pass", std::move(face)}}})}};
        std::ofstream{root / "features" / "rc7e_cube.json", std::ios::binary | std::ios::trunc} << cube.dump(2);
        features.push_back("project://features/rc7e_cube.json");
    }
    J present{{"name", "g1_present"}, {"type", "fullscreen"}, {"input", J::array({used})}, {"after", J::array({"g1_fill"})},
              {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader", {{"vertex", shaders.vertex}, {"fragment", shaders.copy}}}};
    return J{{"features", std::move(features)},
             {"render_targets", std::move(targets)},
             {"rendering_passes",
              J::array({J{{"name", "main_render"},
                          {"passes", J::array({fill("g1_fill", used, shaders.fill_red), std::move(present)})}}})}};
}

} // namespace Pelican::TestSupport
