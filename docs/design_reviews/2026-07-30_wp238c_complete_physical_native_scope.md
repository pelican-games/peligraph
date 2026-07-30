# WP238c complete physical plan / NativeScope review

## Outcome

`pelican.vulkan_complete_physical_plan` version 1 now provides a canonical,
environment-bound, same-layer package for all engine-visible Vulkan physical
resources, ordered scopes, attachments, alias groups, and required features.
It is separate from the sparse physical fragment ABI.

The package can be ejected from a `VulkanTargetPlan`, serialized, parsed,
fingerprinted, and passed through a strict verifier without creating GPU
objects. `vulkanTargetPlanToJson` exposes it as
`ejectable_complete_physical_plan`.

## Verification boundary

Verification rejects:

- a stale logical graph, automatic plan, backend candidate, or target topology;
- missing, duplicated, or dependency-reversed node execution;
- authored lifetime/read-footprint metadata that disagrees with the ordered
  logical uses;
- invalid resource shapes, materialization, sample counts, or feature closure;
- alternate formats without per-resource device evidence;
- invalid attachment subresources and alias groups with incompatible shapes or
  overlapping lifetimes;
- NativeScope boundaries with hidden resource access, wrong semantic types,
  unavailable queue/features/extensions, or incomplete synchronization data.

There is no engine-authored resource-count or MRT-count ceiling in this
package. Device limits remain device-compiler gates.

## NativeScope v1

NativeScope v1 is deliberately data-only. It references one verified physical
scope and declares:

- a versioned implementation identity and opaque object configuration;
- every logical resource effect and semantic type;
- engine or native ownership (native ownership is limited to logical external
  resources in v1);
- queue capability, physical features, Vulkan extensions, and alias groups;
- automatic, manual, or unchecked synchronization;
- capture, device-loss, and hot-reload support.

Automatic synchronization forbids handwritten stage/access masks. Manual mode
requires them for every boundary resource. Unchecked mode remains usable and
emits a stable warning rather than injecting a conservative global barrier.

## Deliberate non-goals

This slice does not register a raw Vulkan callback, freeze a game-DLL command
ABI, create backend-private GPU objects, or publish a NativeScope into the live
renderer. Those operations need an owner/generation-leased executor provider,
prepare-time object ownership, failure-atomic publication, and retirement
fixtures. Keeping that gate separate makes the current package useful for
ejection, tooling, and compiler development without creating an unverified
runtime path.
