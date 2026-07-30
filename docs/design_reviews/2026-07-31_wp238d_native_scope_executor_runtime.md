# WP238d NativeScope executor runtime review

## Outcome

Verified complete Vulkan physical packages can now opt one or more physical
scopes into source-level NativeScope execution. A custom render compiler places
the verified package beside the matching `VulkanTargetPlan`; package validation
rejects target/package drift before mutable GPU registration begins.

The runtime resolves every declaration through an owner-aware executor provider
snapshot, prepares provider-private state during the existing GPU registration
transaction, and publishes that state inside the immutable renderer generation.
The callback records once per physical-scope/view invocation and replaces every
normal render, compute, transfer, output, or marker body covered by that scope.

## Ownership and reload

The provider registry records implementation identity, provider identity,
registration owner/generation, API version, queue capabilities, and capability
bits. Non-engine providers must supply a strong generation lease. A snapshot
and every prepared executor retain the provider entry, with member destruction
ordered so the executor virtual destructor runs before provider code can
unload. The provider registry is activated and retired by the same game-logic
reload transaction as the existing render-policy, pass, transform, subgraph,
and strategy registries.

Prepared executors are part of `CompiledFrameGraphExecution`. Therefore:

- a failed provider prepare or later GPU-registration failure destroys the
  candidate without changing the active generation;
- publication is still one compare/exchange with render programs and the GPU
  arena;
- an in-flight frame retains the old executor after replacement until its
  renderer-generation lease retires;
- provider-owned device objects can be created in `prepare` from the Vulkan
  device and engine allocation/pipeline services, then released by the
  executor state destructor.

This remains a source extension. No raw Vulkan callback ABI was added to the
public game-DLL surface.

## Command and resource boundary

The record context exposes the command buffer, frame context, active
`FrameResources`, view invocation, and only the resources declared by the
NativeScope boundary. Engine-owned resources are lowered to:

- render-target images, including resolved/MSAA attachment images, layered or
  sequential views, and per-node attachment subresources;
- the current frame target;
- frame-graph buffers;
- provider-owned external placeholders.

For `automatic` synchronization, external frame-graph dependencies still run
in the engine and declared images are transitioned to a layout derived from
scope kind, access effect, and attachment aspect. Internal scope dependencies
are not replayed because the provider owns them. `manual` and `unchecked`
providers own their barriers but must return engine-owned images to the
declared outer layout; the layout tracker resumes from that explicit
assumption.

## Runtime observability

`currentFramePlanJson` now reports `native_scope_executors` with complete-plan
fingerprint, scope and implementation identities, provider owner/generation,
capabilities, synchronization mode, queue capability, and lowered resource
kinds. Per-node frame traces report which scope recorded the callback and which
logical nodes were suppressed.

## Verification

The focused test covers:

- built-in empty-marker execution;
- exact provider selection and missing/capability rejection;
- provider snapshot survival after owner retirement;
- in-flight renderer-generation lease retention across replacement;
- complete-package/runtime-plan drift rejection before GPU registration.

The focused NativeScope test passes 23 assertions in 4 cases. The adjacent
render-pipeline transaction, compiler-program, target-planning,
frame-execution-plan, and reload-transaction suites pass 7,508 assertions in
62 cases.

The current built-in provider is intentionally an empty marker fixture.
Follow-up WP238e now supplies the command-producing Vulkan example, real-device
validation/readback capture, and renderer-generation retirement gate without
turning the fixture into a built-in feature. A controlled device-loss injection
and a public, versioned, `noexcept` game-DLL ABI remain separate work. See
[`2026-07-31_wp238e_native_scope_vulkan_fixture.md`](2026-07-31_wp238e_native_scope_vulkan_fixture.md).
