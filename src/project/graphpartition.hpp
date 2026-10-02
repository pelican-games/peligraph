#pragma once

#include "graphcomposition.hpp"
#include "graphmodule.hpp"

namespace Pelican {
struct GraphExecutionPartitions {
    ExpandedGraphDocument values;
    ExpandedGraphDocument rendering;
    // Evaluated inside the animation phase, one step earlier in the same frame
    // than `values`. Every scalar an animation node consumes is published by
    // `values` as a `$pose/<node>/<port>` output, so the feed is one-way.
    ExpandedGraphDocument animation;
};
// The scalar output name the values partition publishes for one animation-node
// input port. Both sides spell it here.
std::string graphAnimationScalarOutput(std::string_view node, std::string_view port);
// The scalar output name the values partition publishes for one instance input
// port, keyed as ExpandedGraphDocument::instance_inputs is. Reading what a
// drawn frame used starts here: a connected port has no node of its own, so it
// is only observable once the expansion names it as an output. Both sides spell
// it here.
std::string graphInstanceInputOutput(std::string_view input);
// Validate the entire authored graph before separating execution domains.
// Connected scalar buffer inputs become CPU publication sinks and host buffers.
// Every operation comes from a registered module; the overload taking a registry
// is how a test, or a build without some module, sees a different set.
GraphExecutionPartitions partitionGraphDocument(const nlohmann::json &document);
GraphExecutionPartitions partitionGraphDocument(const nlohmann::json &document,
                                                const GraphModuleRegistry &modules);
// Resolve only instantiated attribute sources. The returned compiler document
// contains derived counts; the authored document remains unchanged.
using GraphAttributeSourceResolver = std::function<std::uint64_t(
    std::string_view node, const nlohmann::json &parameters)>;
nlohmann::json resolveGraphAttributeSources(const nlohmann::json &document,
    const GraphAttributeSourceResolver &resolve);

// IA-1: a render.image's `initial: {asset: "<reference>"}` is the contents a
// persistent image starts from: copied from the asset (no scaling, no format
// conversion) each time the image is made and on the first frame of a
// runtime generation. Refuses what the node may not say:
//   graph.image_initial_requires_persistent@1 {node}  the image is transient
//   graph.image_initial_extent_conflict@1 {node}      no width / height (the
//       asset's are taken) but an extent_scale
// and, as std::invalid_argument naming the node, an `initial` that is not
// {asset: <non-empty string>} or an image that is not one 2D layer.
void checkRenderImageInitial(const std::string &node, const nlohmann::json &parameters);

// What an initial asset holds, as the engine read it.
struct GraphImageInitialAsset {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The Vulkan name of its pixels ("R32G32B32A32_SFLOAT"); a PNG decodes
    // to "R8G8B8A8_UNORM".
    std::string format;
    // Whether the file states its color space (KTX2 does, PNG does not).
    bool color_space_declared = false;
};
using GraphImageInitialAssetReader =
    std::function<GraphImageInitialAsset(std::string_view node, const std::string &asset)>;

// IA-1: reads each instantiated render.image's initial asset and returns the
// compiler document with the asset's width and height written into an image
// that left them out. An asset that does not fit the image is
//   graph.image_initial_mismatch@1 {node, asset, what, expected, actual}
// with what "extent" (the size), "format" (channels, bits, float or not) or
// "color_space" (the same pixels, but a KTX2 that declares sRGB for a UNORM
// image or the reverse; a PNG declares none and fits either), expected and
// actual {width, height, format}. An image whose format is not a plain name
// (a format class, an expression) is checked by its extent only.
nlohmann::json resolveGraphImageInitialAssets(const nlohmann::json &document,
                                              const GraphImageInitialAssetReader &read);
}
