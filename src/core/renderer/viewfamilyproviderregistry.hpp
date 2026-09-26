#pragma once

#include "viewfamily.hpp"
#include "../../project/renderpipeline.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct CompiledFrameGraphExecution;
class LightContainer;
class RenderTargetContainer;

// Immutable engine inputs shared by runtime view-family providers. Providers
// return logical view data only; the scheduler and backend continue to own
// execution, temporal state, resources, and Vulkan lowering.
struct RenderViewFamilyProviderContext {
    const CompiledFrameGraphExecution
        &frame_graph;
    const LightContainer &lights;
    const RenderTargetContainer
        &render_targets;
};

// LP-Va: what a family's compile-time axis lengths may read. Both spans come
// from the composed pipeline (CompiledRenderPipeline, or the same data
// compiled from the composed JSON at an earlier stage).
struct RenderViewAxisLengthContext {
    std::span<const CompiledRenderFeatureInstance>
        feature_instances;
    std::span<const CompiledSurfaceResourceContract>
        surface_resource_contracts;
};

RenderViewAxisLengthContext renderViewAxisLengthContext(
    const CompiledRenderPipeline &pipeline) noexcept;

// One named integer axis of a provider-owned family. Every view the provider
// builds carries a coordinate for each declared axis in its `axes` map.
// compile_time_length is empty for an axis whose length is only known per
// frame (a light count).
struct RenderViewAxisDefinition {
    std::string name;
    std::function<std::uint32_t(
        const RenderViewAxisLengthContext &)>
        compile_time_length;
    // LP-Va2: display names of the values, label[i] naming value i (Studio
    // shows them next to view_select; the value stays the integer). Empty
    // when the values are plain numbers.
    std::vector<std::string> labels;
};

// LP-Va2: the labels of a `face` axis, in the cube order every face-axis
// provider builds its views in (+X, -X, +Y, -Y, +Z, -Z).
std::vector<std::string> renderViewAxisFaceLabels();

struct RenderViewFamilyProviderDefinition {
    std::string name;
    std::string family_id;
    // Declared in the order a view index is composed (outermost first).
    std::vector<RenderViewAxisDefinition> axes;
    std::function<RenderViewFamily(
        const RenderViewFamilies &,
        const RenderViewFamilyProviderContext &)>
        build;
    std::function<void(
        const RenderViewFamilies &,
        const RenderViewFamilyProviderContext &)>
        validate;
};

// Source-level registry used by engine and optional render-algorithm packages.
// Authored RenderViewFamilies remain the highest-priority replacement path:
// a provider runs only when a graph-required family was not supplied.
class RenderViewFamilyProviderRegistry {
    std::vector<
        RenderViewFamilyProviderDefinition>
        providers_;

  public:
    void registerProvider(
        RenderViewFamilyProviderDefinition
            provider);

    const RenderViewFamilyProviderDefinition *
    find(std::string_view family_id) const
        noexcept;

    const std::vector<
        RenderViewFamilyProviderDefinition> &
    providers() const noexcept {
        return providers_;
    }
};

RenderViewFamilyProviderRegistry &
renderViewFamilyProviderRegistry();

enum class RenderViewAxisLengthKind : std::uint8_t {
    // No registered provider declares this axis for the family. An authored
    // family (no provider) has no axes at all.
    unknown,
    // The axis exists but its length is only known per frame.
    runtime,
    // The axis length is fixed when the render graph is (re)compiled.
    compile_time,
};

struct RenderViewAxisLength {
    RenderViewAxisLengthKind kind =
        RenderViewAxisLengthKind::unknown;
    std::uint32_t length = 0;

    bool operator==(const RenderViewAxisLength &) const = default;
};

// LP-Va / LP-Vb: the length of one family axis as the recompile stage sees
// it. `$capture/cube` face -> compile_time 6, `$shadow/directional` cascade
// -> compile_time cascade_count, any light axis -> runtime.
RenderViewAxisLength renderViewAxisLength(
    std::string_view family_id,
    std::string_view axis,
    const RenderViewAxisLengthContext &context,
    const RenderViewFamilyProviderRegistry &providers =
        renderViewFamilyProviderRegistry());

// LP-Va2: one axis of a family as get_frame_plan reports it.
struct RenderViewAxisDescription {
    std::string name;
    // runtime or compile_time; never unknown (only declared axes are listed).
    RenderViewAxisLength length;
    std::vector<std::string> labels;
};

// LP-Va2: every axis the family's provider declares, in the provider's index
// order (outermost first), each with its length as the recompile stage sees
// it. Empty for `$main`, `$reflection/planar` and a family without a
// provider.
std::vector<RenderViewAxisDescription> renderViewFamilyAxes(
    std::string_view family_id,
    const RenderViewAxisLengthContext &context,
    const RenderViewFamilyProviderRegistry &providers =
        renderViewFamilyProviderRegistry());

struct FrameGraphDefinition;

// Recompile-time check of every `view_select` in the graphs against the
// providers' axes. Throws a named refusal (project/viewselect.hpp):
// view_select_primary_family, view_select_axis_unknown (also for a value
// outside a compile-time length, with `value` and `length`), or
// view_select_runtime_axis.
void validateRenderViewSelects(
    std::span<const FrameGraphDefinition> graphs,
    const CompiledRenderPipeline &pipeline,
    const RenderViewFamilyProviderRegistry &providers =
        renderViewFamilyProviderRegistry());

RenderViewFamilies resolveRuntimeRenderViewFamilies(
    const RenderViewFamilies &authored,
    const RenderViewFamilyProviderContext
        &context,
    const RenderViewFamilyProviderRegistry
        &providers =
            renderViewFamilyProviderRegistry());

// Call after generic RenderViewFamilies validation so provider callbacks can
// assume structurally valid family/view identities.
void validateRuntimeRenderViewFamilyProviderContracts(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context,
    const RenderViewFamilyProviderRegistry
        &providers =
            renderViewFamilyProviderRegistry());

} // namespace Pelican
