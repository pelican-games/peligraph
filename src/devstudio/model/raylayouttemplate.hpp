#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PelicanStudio {

// RTL-2s / RTL-2s-c: the one set of shader templates of a project with a ray
// group layout. The Graph tab's New... (one stage for one place), the Ray
// layout section's Enable hit groups and its "new shader from the template"
// (a hit group, a ray type, a miss) all write these. Qt-free.
//
// Each stage is written for one place of a task's ray_tracing (its raygen,
// one miss, one cell of hit_shaders, a callable), in the form the layout's
// generated header serves:
//   - every stage includes "__pelican_ray_group_layout.glsl" (the engine
//     generates it from the layout and gives it to the layout's tasks);
//   - the payload of ray type 0 (location 0) is a vec4: rgb the colour its
//     closest hit gives, w the distance to the hit -- greater than 0 on a hit,
//     0 when the miss ran. Ray type i > 0's (location i) is a float, 1 when
//     something lies between the point and the light (a shadow ray). Miss i
//     answers ray type i's payload;
//   - raygen traces the first ray type with PELICAN_RAY_TYPE_<name>,
//     pelican_ray_type_count and PELICAN_MISS_<name> of the first miss; with
//     a second ray type it holds, behind `#define TRACE_SECOND_RAY_TYPE`, a
//     ray of that type toward a light, and behind `#define CALL_CALLABLE_0`
//     the colour passed through callable 0 (setRayLayoutRaygenSwitch turns
//     them on and off);
//   - a closest hit of ray type 0 colours by the surface normal --
//     pelicanRayFetchNormal when the layout's engine_record has
//     geometry_index, else the position it hit -- a group other than the
//     default one tints it, and the record's first field is added to it
//     (recordColor); one of a later ray type says "occluded";
//   - any hit accepts every candidate; intersection is a sphere of radius 1
//     at the procedural geometry's origin (or the record's `center` /
//     `radius`); callable tints the colour it is given.
// A hit shader that reads the shader record declares it in a block Studio
// owns, between `// pelican: shader record` and `// pelican: end shader
// record`: the engine's fields (PELICAN_RAY_ENGINE_RECORD_MEMBERS), then the
// record's in order, and recordColor(). RTL-3s (engine EB-3): a hit shader of
// a traced ray type may declare no record, and one that declares it declares
// exactly that one, so Studio writes the block again whenever the layout's
// record changes (rewriteRayRecordBlock); the templates that read no record
// (a later ray type's closest hit, any hit, an intersection without the
// record's center / radius) hold no block.
struct RayLayoutTemplateNames {
    std::vector<std::string> ray_types;
    std::vector<std::string> misses;
    // (name, "triangles" | "procedural"), in the layout's order.
    std::vector<std::pair<std::string, std::string>> groups;
    std::string default_group;
    std::vector<std::string> engine_record;
    // (name, i32 | u32 | f32 | vec2 | vec3 | vec4), in the layout's order.
    std::vector<std::pair<std::string, std::string>> record;
};
// The names as a pelican.ray_group_layout document holds them; nullopt for
// anything that is not one (no ray type).
[[nodiscard]] std::optional<RayLayoutTemplateNames>
rayLayoutTemplateNamesFromDocument(const nlohmann::json &document);

enum class RayLayoutTemplateStage { raygen, miss, closest_hit, any_hit, intersection, callable };
// ".rgen", ".rmiss", ".rchit", ".rahit", ".rint", ".rcall".
[[nodiscard]] std::string_view rayLayoutTemplateExtension(RayLayoutTemplateStage stage);
// The place of the task's ray_tracing a shader is written for: the ray type
// and group of a hit_shaders cell, the miss of a misses row. A raygen's
// second ray type and its miss (empty: the layout's second ones), and
// whether its two switches start on.
struct RayLayoutTemplatePlace {
    RayLayoutTemplateStage stage = RayLayoutTemplateStage::raygen;
    std::string ray_type;
    std::string group;
    std::string miss;
    std::string second_ray_type = {};
    std::string second_miss = {};
    bool trace_second = false;
    bool call_callable = false;
};
[[nodiscard]] std::string rayLayoutShaderTemplate(const RayLayoutTemplateNames &layout,
                                                  const RayLayoutTemplatePlace &place,
                                                  std::string_view name);

// The raygen's two switches.
inline constexpr std::string_view RayTraceSecondSwitch = "TRACE_SECOND_RAY_TYPE";
inline constexpr std::string_view RayCallCallableSwitch = "CALL_CALLABLE_0";
// Whether `source` has `#define <name> 1` (true), `0` (false); nullopt
// without the line (a raygen Studio did not write).
[[nodiscard]] std::optional<bool> rayLayoutRaygenSwitch(std::string_view source, std::string_view name);
// `source` with that line's value set; nullopt without the line.
[[nodiscard]] std::optional<std::string> setRayLayoutRaygenSwitch(std::string_view source,
                                                                 std::string_view name, bool on);

// The block Studio owns: the shader record of `layout` and recordColor().
[[nodiscard]] std::string rayRecordBlock(const RayLayoutTemplateNames &layout);
// Whether `source` holds that block (between its two marker lines).
[[nodiscard]] bool hasRayRecordBlock(std::string_view source);
// Whether `source` declares shader-record storage at all (shaderRecordEXT).
[[nodiscard]] bool declaresRayShaderRecord(std::string_view source);
// `source` with its block written for `layout` (the line ending kept);
// nullopt when it holds none.
[[nodiscard]] std::optional<std::string> rewriteRayRecordBlock(std::string_view source,
                                                              const RayLayoutTemplateNames &layout);

} // namespace PelicanStudio
