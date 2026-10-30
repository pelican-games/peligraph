#pragma once

// RC-3c2a: the generation's resource description (docs/design_render_graph_ir.md
// §2.4): one record of every config image and buffer a published generation
// registers, merged across its runtime variants, with the engine's usage and
// the extent rule applied once. The resource checks and the GPU registration
// read it (core makes its RenderTargetDefinition / RenderTargetMetadata from a
// GenerationImage, and registers the buffers as they are here).

#include "framegraphbufferdefinition.hpp"
#include "imageformat.hpp"
#include "imageusage.hpp"
#include "rendergraphir.hpp"
#include "renderpipeline.hpp"
#include "rendertargetstoragemode.hpp"
#include "resourceextent.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// ---- the engine's usage (one definition; two points apply it) ------------
//
// What the engine adds to an image's declared usage for what the physical
// plan decided of it. The physical plan's queries ask it of a candidate
// before the plan (one variant, the storage being asked about); the
// generation's description asks it once of the merged image, after the
// storage of every variant is merged (so a storage none kept leaves no bit).
struct ImageUsageFacts {
    ImageUsage declared;
    bool history = false;
    RenderTargetStorageMode storage = RenderTargetStorageMode::materialized;
    // A variant's plan reads the image as a local-read input attachment.
    bool local_read = false;
    // A variant's plan exports the image as the external depth (XR).
    bool external_depth_source = false;
};

// The declared bits, INPUT_ATTACHMENT for a local read and TRANSFER_SRC for
// an external depth source; then a transient attachment adds
// TRANSIENT_ATTACHMENT and a tile-local one keeps only its attachment bits
// with INPUT_ATTACHMENT and TRANSIENT_ATTACHMENT; a history image adds
// TRANSFER_DST (it is cleared through a copy).
ImageUsage engineImageUsage(const ImageUsageFacts &facts);

// What a use of an image requires of its declared usage: the graph lowering
// completes a graph image's declaration with these, and the checks of the
// uses (a snapshot copy's two ends) require them.
enum class ImageUseRequirement : std::uint8_t {
    // A pass samples it.
    shader_input,
    // A snapshot copy reads from it.
    copy_source,
    // A snapshot copy writes it (and it is sampled after).
    copy_destination,
    // It holds an asset before any pass runs (`initial`).
    initial_contents,
    // It is read from a file (`source`); its mips, when generated, are
    // blitted from the first level.
    file_source,
    file_source_mips,
};
ImageUsage requiredImageUsage(ImageUseRequirement use);

// ---- the description ------------------------------------------------------

// One variant's physical plan for one config image (the neutral part of what
// the plan assigned: core's RenderingTargetPlanCompilation, extracted).
struct ImagePlacement {
    std::string resource;
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;
    std::uint32_t samples = 1;
    std::uint32_t array_layers = 1;
    RenderTargetStorageMode storage = RenderTargetStorageMode::materialized;
    std::optional<std::string> alias_group;
    bool local_read = false;
    bool external_depth_source = false;

    bool operator==(const ImagePlacement &) const = default;
};

// One runtime variant: its final IR (the config's images and buffers), its
// placements (exactly one per config image), and the images whose layer
// count a runtime view count decides (the shadow view families'; their
// layers here are the plan's, a static floor).
struct GenerationResourceVariant {
    std::string name;
    const RenderGraphIR *render_graph = nullptr;
    std::vector<ImagePlacement> placements;
    std::vector<std::string> runtime_layered_images;
};

struct GenerationImage {
    // The declaration (the first variant's), its usage the union of the
    // variants' declared usage.
    ImageResource declaration;
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;
    // The final usage (engineImageUsage over the merged facts).
    ImageUsage usage;
    // resolveResourceExtent(declaration.extent, output extent).
    ResolvedResourceExtent extent;
    std::uint32_t samples = 1;
    std::uint32_t array_layers = 1;
    RenderTargetStorageMode storage = RenderTargetStorageMode::materialized;
    std::optional<std::string> alias_group;
    // Some variant keeps its contents (`lifetime: persistent`).
    bool persistent = false;
    // Static constraint: the layer count follows a runtime view count (the
    // runtime resizes it and checks the views against it), so the image is
    // never aliased, never history, 2D and not read from a file.
    bool runtime_array_layers = false;

    bool operator==(const GenerationImage &) const = default;
};

struct GenerationResources {
    std::vector<GenerationImage> images;
    // Why a proposed runtime allocation was split after variant merging.
    std::map<std::string, std::string, std::less<>> alias_rejections;
    // Sized (a size_from_extent buffer from its image's resolved extent) and
    // held to their size contract.
    std::vector<FrameGraphBufferDefinition> buffers;
    // Each variant's images and buffers, by index into the above, in its
    // declaration order (the order it registers them in).
    struct Variant {
        std::vector<std::size_t> images;
        std::vector<std::size_t> buffers;

        bool operator==(const Variant &) const = default;
    };
    std::vector<Variant> variants;

    const GenerationImage *findImage(std::string_view name) const;
    const FrameGraphBufferDefinition *findBuffer(std::string_view name) const;

    bool operator==(const GenerationResources &) const = default;
};

// §2.4. Merges the variants' images: usage is the union, layers the maximum,
// storage materialized where the variants differ; format, mip levels,
// dimension, extent, samples and history must agree. An alias group stands
// when every variant has all of its members or none of them, and each that
// has them places them in it; otherwise every member leaves it. Each group
// left has two members or more and one contract. Buffers of one name must
// agree in every field. Refuses by name:
//   render_graph.image_variant_conflict@1   {resource, field, variants}
//       field: format | mip_levels | dimension | extent | samples | history | source
//   render_graph.image_placement_invalid@1  {variant, resource, reason}
//       a placement of no config image, a config image without one (no
//       placements at all included), or one of an image twice
//   render_graph.alias_group_invalid@1      {group, reason, resource?}
//       reason: member_count | contract
//   render_graph.image_runtime_layers_invalid@1 {resource, reason}
//       an image whose layers a runtime view count decides that keeps
//       history, is a cube or is read from a file
//   render_graph.buffer_variant_conflict@1  {resource, field, variants}
//   render_graph.buffer_invalid@1           {resource, field, reason}
//       a size from an extent that names no config image (unknown_image),
//       breaks its size contract (requireBufferSizeContract) or overflows
GenerationResources resolveGenerationResources(std::span<const GenerationResourceVariant> variants,
                                               ResolvedResourceExtent output_extent);

// RC-3c2a: the resource checks of a pass, from its IR uses and the
// generation's description (the values the GPU registration takes), before
// anything is registered:
//   render_graph.image_usage_missing@1 {graph, node, resource, usage}
//       an attachment without COLOR_ATTACHMENT / DEPTH_STENCIL_ATTACHMENT, a
//       shader read (input, screen input, surface resource, image material
//       resource) without SAMPLED or a tile-local INPUT_ATTACHMENT
//   render_graph.history_input_invalid@1 {graph, node, resource}
//       an @history read of an image that keeps no history
//   render_graph.attachment_resource_invalid@1 {graph, node, resource, reason}
//       reason: subresource | extent | samples (the pass's attachments are
//       of one extent, at their mip, and one sample count; the swapchain's
//       is 1)
//   render_graph.snapshot_invalid@1 {graph, node, resource, reason}
//       reason: unknown_resource | usage | format | extent
//   render_graph.material_input_invalid@1 {graph, node, resource, port, detail}
//       a screen input or surface resource whose image's type its contract
//       does not take (materialInputSourceType)
//   render_graph.material_attachment_invalid@1 {graph, node, reason, ...}
//       reason: outputs_require_contract | scene_color_in_deferred |
//       color_count {expected} | requires_depth | swapchain_color |
//       numeric_class {resource, output} | color_format {resource,
//       expected} | depth_format {resource, expected} | unknown_resource
// Compute tasks, acceleration-structure builds and anchors are not checked
// here (a compute task's buffers: validateComputeTaskBufferContracts).
//
// What a use names is decided by `render_graph`, the variant's own IR: a
// name that is not one of its config images (a buffer, the swapchain) is
// never looked up in `resources`, though another variant may declare an
// image of that name. `observe`, when set, receives every image the checks
// read from `resources`, as they read it.
struct ResourceCheckRead {
    std::string_view graph;
    std::string_view node;
    const GenerationImage &image;
};
using ResourceCheckObserver = std::function<void(const ResourceCheckRead &)>;
void validateRenderNodeResources(const RenderGraphIR &render_graph, const GraphIR &graph, const NodeIR &node,
                                 const GenerationResources &resources, const ResourceCheckObserver &observe = {});
void validateRenderGraphResources(const RenderGraphIR &render_graph, const GenerationResources &resources,
                                  const ResourceCheckObserver &observe = {});

// The editor's question, by its own entry: one variant of `render_graph`
// with each image as declared (single-sample, materialized, unaliased, its
// declared format and layers) -- no physical plan.
GenerationResources resolveDeclaredGenerationResources(const RenderGraphIR &render_graph,
                                                      ResolvedResourceExtent output_extent);

// The images whose layer count a runtime view count decides: the targets of
// the directional, point and spot shadow contracts (their view families
// resize them by the light count).
std::vector<std::string> runtimeLayeredImagesOf(const CompiledRenderPipeline &pipeline);

} // namespace Pelican
