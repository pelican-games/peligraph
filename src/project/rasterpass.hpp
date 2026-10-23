#pragma once

#include "materialoutput.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
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

// The depth compare, and the stencil test's compare.
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

// PSN-1: the rest of the graphics-pipeline state groups, in the same
// portable spelling (design_pipeline_state_nodes.md §2).
enum class RasterPolygonMode : std::uint8_t {
    fill,
    line,
    point,
};

std::string_view rasterPolygonModeName(RasterPolygonMode mode);

enum class RasterConservativeMode : std::uint8_t {
    off,
    overestimate,
    underestimate,
};

std::string_view rasterConservativeModeName(
    RasterConservativeMode mode);

enum class RasterStencilOperation : std::uint8_t {
    keep,
    zero,
    replace,
    increment_and_clamp,
    decrement_and_clamp,
    invert,
    increment_and_wrap,
    decrement_and_wrap,
};

std::string_view rasterStencilOperationName(
    RasterStencilOperation operation);

// Spelled and, xor, or, ... in JSON (bitwise_* here: those are C++ tokens).
enum class RasterLogicOperation : std::uint8_t {
    clear,
    bitwise_and,
    and_reverse,
    copy,
    and_inverted,
    no_op,
    bitwise_xor,
    bitwise_or,
    nor,
    equivalent,
    invert,
    or_reverse,
    copy_inverted,
    or_inverted,
    nand,
    set,
};

std::string_view rasterLogicOperationName(
    RasterLogicOperation operation);

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

// `depth_bounds [min, max]`: the depth bounds test keeps the fragments whose
// stored depth lies in the range.
struct RasterDepthBounds {
    double min = 0.0;
    double max = 1.0;

    bool operator==(
        const RasterDepthBounds &) const = default;
};

struct RasterDepthState {
    bool test = false;
    bool write = false;
    RasterDepthCompare compare =
        RasterDepthCompare::less;
    // Engaged: the depth bounds test (device depthBounds).
    std::optional<RasterDepthBounds> bounds;

    bool operator==(
        const RasterDepthState &) const = default;
};

// `depth_bias {constant, slope, clamp}`: Vulkan's depthBiasConstantFactor,
// depthBiasSlopeFactor and depthBiasClamp. A non-zero clamp needs the
// device's depthBiasClamp.
struct RasterDepthBias {
    double constant = 0.0;
    double slope = 0.0;
    double clamp = 0.0;

    bool operator==(
        const RasterDepthBias &) const = default;
};

// Every stencil format the engine names has 8 stencil bits.
inline constexpr std::uint32_t rasterStencilValueMax = 0xffu;
inline constexpr std::uint32_t rasterSampleMaskAll = 0xffffffffu;

struct RasterStencilFaceState {
    RasterStencilOperation fail =
        RasterStencilOperation::keep;
    RasterStencilOperation pass =
        RasterStencilOperation::keep;
    RasterStencilOperation depth_fail =
        RasterStencilOperation::keep;
    RasterDepthCompare compare =
        RasterDepthCompare::always;
    std::uint32_t compare_mask = rasterStencilValueMax;
    std::uint32_t write_mask = rasterStencilValueMax;
    std::uint32_t reference = 0;

    bool operator==(
        const RasterStencilFaceState &) const = default;
};

struct RasterStencilState {
    RasterStencilFaceState front;
    RasterStencilFaceState back;

    bool operator==(
        const RasterStencilState &) const = default;
};

// PSN-1: the one value of a graphics pipeline's fixed-function state, by
// Vulkan's state groups. Pure logic: the project parses it (one JSON reader,
// parsePipelineState) and the backend lowers it (one Vulkan mapping,
// applyVulkanPipelineState in rasterpassvulkanadapter.hpp). A default value
// is the engine's default (cull none, depth test off, line width 1, no
// stencil, ...), not Vulkan's, so a document that writes none of it draws as
// before. Pass definitions carry it whole.
struct PipelineState {
    // input assembly
    RasterPrimitiveTopology topology =
        RasterPrimitiveTopology::triangle_list;
    // Restarts a strip at the all-ones index of an indexed draw. Strip
    // topologies only.
    bool primitive_restart = false;

    // rasterization
    RasterCullMode cull = RasterCullMode::none;
    RasterFrontFace front_face =
        RasterFrontFace::counter_clockwise;
    RasterPolygonMode polygon_mode =
        RasterPolygonMode::fill;
    bool depth_clamp = false;
    std::optional<RasterDepthBias> depth_bias;
    double line_width = 1.0;
    bool rasterizer_discard = false;
    RasterConservativeMode conservative =
        RasterConservativeMode::off;

    // multisample
    // Engaged: sample shading with this minimum fraction, in [0, 1].
    std::optional<double> sample_shading;
    std::uint32_t sample_mask = rasterSampleMaskAll;
    bool alpha_to_coverage = false;
    bool alpha_to_one = false;

    // depth / stencil
    RasterDepthState depth;
    // Engaged: the stencil test, on the stencil component of the pass's
    // depth attachment (D24_UNORM_S8_UINT).
    std::optional<RasterStencilState> stencil;

    // color blend
    // Engaged: a logical operation replaces blending on every attachment.
    std::optional<RasterLogicOperation> logic_op;
    std::array<double, 4> blend_constants{};
    // Canonical contracts contain one entry per color attachment. There is no
    // engine-side attachment-count ceiling.
    std::vector<RasterColorAttachmentState>
        color_attachments;

    bool operator==(
        const PipelineState &) const = default;
};

// The name the generic raster and fullscreen pass definitions use.
using RasterFixedFunctionState = PipelineState;

// PSN-1: the one JSON reader of the vocabulary. `raster_state` is the inline
// object (every key optional; a missing key keeps PipelineState{}'s value).
PipelineState parsePipelineState(
    const nlohmann::json &raster_state,
    std::size_t color_attachment_count,
    std::string_view context = "raster_state");

// The keys a surface's render_state takes from the vocabulary. Its own
// blend, cull and depth keys stay the surface's (surfaceformat.cpp), and a
// material has no topology or per-attachment state of its own here.
std::span<const std::string_view> surfacePipelineStateKeys() noexcept;

// The same reader over those keys only.
PipelineState parseSurfacePipelineState(
    const nlohmann::json &fields,
    std::string_view context);

// The canonical raster_state object: parsePipelineState reads it back to an
// equal value.
nlohmann::json pipelineStateToJson(const PipelineState &state);

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

// PSN-1: the checks that need no attachment count: value ranges and
// combinations, and that depth bias, depth bounds and stencil have a depth
// attachment to work on. A state checked here may still enable depth test or
// write without one; validateRasterFixedFunctionState refuses that for the
// raster and fullscreen grammars.
void validatePipelineState(
    const PipelineState &state,
    bool has_depth_attachment,
    std::string_view context = "pipeline state");

// PSN-1: a stencil test needs the stencil component of the depth attachment.
// The physical format is known only when the pipeline is lowered; the
// lowering passes what it knows (depth_format is its name, for the message).
void validatePipelineStateStencilFormat(
    const PipelineState &state,
    bool depth_format_has_stencil,
    std::string_view depth_format,
    std::string_view context);

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

// PSN-1: the device facts the vocabulary depends on: the optional features
// the device has (and the engine enabled), read from the runtime capability
// set by the backend (pipelineStateDeviceFacts). Default: none.
struct PipelineStateDeviceFacts {
    bool fill_mode_non_solid = false;
    bool wide_lines = false;
    bool depth_clamp = false;
    bool depth_bias_clamp = false;
    bool depth_bounds = false;
    bool sample_rate_shading = false;
    bool alpha_to_one = false;
    bool logic_op = false;
    bool dual_src_blend = false;
    std::uint32_t max_fragment_dual_src_attachments = 0;
    // VK_EXT_conservative_rasterization and two of its properties.
    bool conservative_rasterization = false;
    bool primitive_underestimation = false;
    bool conservative_point_and_line_rasterization = false;

    bool operator==(
        const PipelineStateDeviceFacts &) const = default;
};

// `pipeline.state_unsupported@1 {pass, field, feature}`: the document writes
// state (field: its raster_state key) that needs a device feature (feature:
// Vulkan's name for it) the device does not have.
inline constexpr std::string_view pipelineStateUnsupportedCode =
    "pipeline.state_unsupported@1";

struct PipelineStateRequirement {
    std::string field;
    std::string feature;

    bool operator==(
        const PipelineStateRequirement &) const = default;
};

// The first requirement of the state the device lacks, in vocabulary order.
std::optional<PipelineStateRequirement>
firstUnsupportedPipelineState(
    const PipelineState &state,
    const PipelineStateDeviceFacts &facts);

// "pipeline.state_unsupported@1: {"pass":...,"field":...,"feature":...}",
// with "material" after them when a material's surface wrote the state.
std::string pipelineStateUnsupportedMessage(
    std::string_view pass,
    const PipelineStateRequirement &requirement,
    std::string_view material = {});

// Throws pipelineStateUnsupportedMessage for the first unsupported
// requirement, at rendering-config registration.
void requirePipelineStateSupported(
    const PipelineState &state,
    const PipelineStateDeviceFacts &facts,
    std::string_view pass);

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
