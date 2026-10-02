#pragma once

#include "materialoutput.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican {

inline constexpr std::string_view rasterPassContractId =
    "pelican.raster.pass@1";
inline constexpr std::string_view authoredDirectRasterImplementationId =
    "pelican.raster.authored_direct@1";

// These are portable raster semantics, not Vulkan spellings. Backends lower
// the finite state to their native pipeline representation.
enum class RasterPrimitiveTopology : std::uint8_t {
    point_list,
    line_list,
    line_strip,
    triangle_list,
    triangle_strip,
};

std::string_view rasterPrimitiveTopologyName(
    RasterPrimitiveTopology topology);

enum class RasterCullMode : std::uint8_t {
    none,
    front,
    back,
};

std::string_view rasterCullModeName(RasterCullMode mode);

enum class RasterFrontFace : std::uint8_t {
    counter_clockwise,
    clockwise,
};

std::string_view rasterFrontFaceName(RasterFrontFace face);

// CC-2: which way the vertex shader's clip +y points on the output. `down` is
// the engine's (Vulkan's) convention; `up` flips the pass's viewport so a
// shader written for a y-up clip space draws upright. With either value,
// front_face and cull mean the winding seen on the finished image.
enum class RasterClipY : std::uint8_t {
    down,
    up,
};

std::string_view rasterClipYName(RasterClipY clip_y);

enum class RasterDepthCompare : std::uint8_t {
    never,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

std::string_view rasterDepthCompareName(
    RasterDepthCompare compare);

// The operation is intentionally typed separately from the open
// implementation id. A replaceable algorithm may emit this operation without
// teaching the backend about the algorithm itself. More native operation
// shapes (indirect/mesh) can be added to this variant later.
struct RasterDirectDrawOperation {
    std::uint32_t vertex_count = 3;
    std::uint32_t instance_count = 1;
    std::uint32_t first_vertex = 0;
    std::uint32_t first_instance = 0;

    bool operator==(
        const RasterDirectDrawOperation &) const = default;
};

struct RasterIndexedDrawOperation {
    std::string index_buffer;
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 1;
    std::uint32_t first_index = 0;
    std::uint32_t first_instance = 0;

    bool operator==(const RasterIndexedDrawOperation &) const = default;
};

using RasterDrawOperation =
    std::variant<RasterDirectDrawOperation, RasterIndexedDrawOperation>;

struct RasterGeometryContract {
    // Open, versioned provenance for the algorithm that produced operation.
    // The backend consumes operation, not this identifier.
    std::string implementation{
        authoredDirectRasterImplementationId};
    RasterDrawOperation operation{
        RasterDirectDrawOperation{}};

    bool operator==(
        const RasterGeometryContract &) const = default;
};

struct RasterColorAttachmentState {
    MaterialOutputBlendState blend;
    std::uint8_t write_mask =
        materialOutputWriteRgba;

    bool operator==(
        const RasterColorAttachmentState &) const = default;
};

struct RasterDepthState {
    bool test = false;
    bool write = false;
    RasterDepthCompare compare =
        RasterDepthCompare::less;

    bool operator==(
        const RasterDepthState &) const = default;
};

struct RasterFixedFunctionState {
    RasterPrimitiveTopology topology =
        RasterPrimitiveTopology::triangle_list;
    RasterCullMode cull = RasterCullMode::none;
    RasterFrontFace front_face =
        RasterFrontFace::counter_clockwise;
    RasterDepthState depth;
    // Canonical contracts contain one entry per color attachment. There is no
    // engine-side attachment-count ceiling.
    std::vector<RasterColorAttachmentState>
        color_attachments;

    bool operator==(
        const RasterFixedFunctionState &) const = default;
};

// Parses only the portable fixed-function state. This is intentionally
// independent from RasterGeometryContract so procedural fullscreen passes do
// not need to manufacture a draw contract.
RasterFixedFunctionState parseRasterFixedFunctionState(
    const nlohmann::json &pass,
    std::size_t color_attachment_count,
    std::string_view context = "raster pass");

// Fullscreen and output-transform passes share the portable raster grammar,
// but deliberately do not own a depth attachment.  Keep the JSON-level depth
// prohibition beside the shared parser so project resolution and runtime
// parsing reject the same authored document.
RasterFixedFunctionState parseFullscreenRasterFixedFunctionState(
    const nlohmann::json &pass,
    std::string_view context = "fullscreen pass");

void validateRasterFixedFunctionState(
    const RasterFixedFunctionState &state,
    std::size_t color_attachment_count,
    bool has_depth_attachment,
    std::string_view context = "raster pass");

// Physical target formats are known only during runtime lowering. Keep this
// validation shared by generic raster and fullscreen paths so integer render
// targets fail before shader registration and Vulkan pipeline creation.
void validateRasterColorAttachmentNumericClasses(
    std::span<const RasterColorAttachmentState> states,
    std::span<const MaterialOutputNumericClass> numeric_classes,
    std::string_view context);

struct RasterPassContract {
    std::uint32_t schema_version = 1;
    RasterGeometryContract geometry;
    RasterFixedFunctionState state;
    // A pass-level field, not raster_state: fullscreen passes share
    // raster_state and their clip space is the engine's.
    RasterClipY clip_y = RasterClipY::down;

    bool operator==(
        const RasterPassContract &) const = default;
};

RasterPassContract parseRasterPassContract(
    const nlohmann::json &pass,
    std::size_t color_attachment_count,
    bool has_depth_attachment,
    std::string_view context = "raster pass");

void validateRasterPassContract(
    const RasterPassContract &contract,
    std::size_t color_attachment_count,
    bool has_depth_attachment,
    std::string_view context = "raster pass");

nlohmann::json rasterPassContractToJson(
    const RasterPassContract &contract);

// Canonical, human-readable cache/reload identity.
std::string rasterPassContractFingerprint(
    const RasterPassContract &contract);

} // namespace Pelican
