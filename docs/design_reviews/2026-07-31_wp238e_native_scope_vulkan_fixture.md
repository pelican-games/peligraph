# WP238e NativeScope Vulkan execution fixture review

## Outcome

The source-level NativeScope seam now has a command-producing Vulkan fixture.
A delegating render compiler starts from the default Vulkan result, replaces
one isolated physical scope with a verified complete physical package, and
installs a provider that records a dynamic-rendering clear into the declared
color attachment. The following fullscreen pass samples that attachment and
the headless target readback proves that the native command affected the final
frame.

This is deliberately a test provider rather than a new built-in rendering
feature. It exercises the same compiler, registry, preparation, publication,
recording, and retirement path available to a source-level engine extension
without adding production code that a game must carry.

## Complete-plan installation boundary

`compileRenderingTargetPlans` now retains an exact verification context beside
each automatic target plan:

- the canonical compiled logical graph;
- the target topology and device facts used by lowering;
- the untouched automatic plan;
- the probed physical format capabilities;
- the exact Vulkan device-extension list enabled by logical-device creation.

`installVerifiedVulkanCompletePhysicalPlanPackage` consumes that retained
context. A delegating compiler no longer reconstructs private device facts or
calls the verifier with an approximation. Installation rejects a missing
graph, a stale or already-replaced automatic plan, a duplicate complete
package, graph/topology/fingerprint drift, an unsupported format, and a
NativeScope extension requirement that is not in the actual enabled-device
closure. Only after strict verification succeeds does it atomically replace
both target-plan indexes and retain the verified marker.

The device-extension closure is captured by `VulkanManageCore` from the list
actually passed to `vkCreateDevice`; it is not inferred later from extensions
merely supported by the physical device.

## Vulkan command fixture

The fixture declares an `R8G8B8A8_UNORM` intermediate color target and isolates
the producer scope. Its provider:

- receives only the typed resource declared at the NativeScope boundary;
- validates a single-sample color-attachment view and extent;
- records `vkCmdBeginRendering` / `vkCmdEndRendering` with a clear load
  operation and store operation;
- emits a scoped Vulkan debug label;
- lets the normal downstream fullscreen pass sample the result.

The first compiler generation clears magenta and readback observes
`RGBA(255, 0, 255, 255)`. A second compiler generation clears green and
readback observes `RGBA(0, 255, 0, 255)`.

## Validation, capture, and lifecycle gate

When `VK_EXT_debug_utils` is enabled, the test installs an error-severity
messenger before the custom plan is compiled and records every validation
message through execution and retirement. The tested run reports zero Vulkan
validation errors. Headless RGBA readback is the deterministic capture gate;
it validates actual image contents rather than only plan metadata or callback
counts.

Two replacement generations are published. Explicit snapshots and submitted
frames keep the previous executor alive, then rotating all in-flight
offscreen slots releases it only after the corresponding GPU work is complete.
The executor destruction counters prove both old generations retire exactly
once.

The declaration and provider advertise device-loss recovery capability, and
their objects use the same generation rebuild/retirement path needed after a
device reconstruction. This work does **not** inject a real
`VK_ERROR_DEVICE_LOST`; that remains a separate destructive or mock-device
gate.

## Verification

Focused results:

- WP238e headless Vulkan fixture: 26 assertions in 1 case;
- render compiler program: 19 assertions in 2 cases;
- target render planning: 300 assertions in 34 cases;
- NativeScope executor runtime: 23 assertions in 4 cases.

The fixture lives in a separate translation unit of the existing headless
integration executable. This keeps MSVC below the default COFF section limit
and limits future recompilation without adding another GPU test executable or
duplicating runtime linkage.

## Remaining boundary

WP238e is enough to validate source-level custom Vulkan command ownership.
It does not freeze a public game-DLL callback ABI. Before such an ABI is added,
the remaining design work is:

1. choose a versioned, `noexcept` C facade and explicit error/result transport;
2. define which Vulkan handles and allocation/pipeline services cross a DLL
   generation boundary;
3. add a controlled device-loss reconstruction test;
4. repeat the fixture with a non-trivial resource or synchronization pattern
   only if that exposes a missing contract.
