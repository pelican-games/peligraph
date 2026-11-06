#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

class VulkanExtensionRequests;
class VulkanManageCore;

// VO-5a: can Spout's GPU path work here? Nothing goes through it yet.
//
// `available` means only that the selected Vulkan device passes the static
// preflight for the proposed D3D11-to-Vulkan import path. No handle has been
// imported and nothing has been synchronized, so this report alone is not
// permission to switch from the CPU path to the GPU path. VO-5b's direction:
// a D3D11 bridge texture on the device spoutDX uses, imported into Vulkan,
// written by Vulkan, then spoutDX::SendTexture. It must still use
// VkExternalMemoryImageCreateInfo, vkGetMemoryWin32HandlePropertiesKHR, the
// handle's memory type bits and VkMemoryDedicatedAllocateInfo when required,
// check the real texture's size and creation parameters, and run a complete
// synchronization protocol.
//
// Spout 2.007h's own shared texture is a legacy KMT handle with a named mutex;
// nothing in that SDK makes an NT handle. An NT bridge would need D3D11's
// sharing flags and IDXGIResource1::CreateSharedHandle.

inline constexpr const char *output_spout_external_memory_extension =
    "VK_KHR_external_memory_win32";
inline constexpr const char *output_spout_external_semaphore_extension =
    "VK_KHR_external_semaphore_win32";
inline constexpr const char *output_spout_keyed_mutex_extension = "VK_KHR_win32_keyed_mutex";

// Three optional device rows, only for --output-spout. No instance row: the
// queries below and VkPhysicalDeviceIDProperties are core in Vulkan 1.1. Having
// them enabled also lets a NativeScope that declares one of them pass its
// requirement check, in runs with the flag.
void registerOutputSpoutExtensionRequests(bool output_spout,
                                          VulkanExtensionRequests &requests);

enum class OutputSpoutHandleKind : std::uint8_t { nt, kmt };

// VK_FORMAT_B8G8R8A8_UNORM, 2D, optimal, usage TRANSFER_DST, one handle kind.
struct OutputSpoutImportQuery {
    // The VkResult's name.
    std::string result;
    // A result other than success or VK_ERROR_FORMAT_NOT_SUPPORTED.
    bool query_failed = false;
    bool importable = false;
    bool dedicated_only = false;
    std::uint32_t max_width = 0;
    std::uint32_t max_height = 0;
};

struct OutputSpoutFormatQuery {
    bool transfer_dst = false;
    // Reported, not required: a BGRA8 to BGRA8 copy doesn't blit. A blit for an
    // RGBA8 source is VO-5b's to validate, with its real formats and usage.
    bool blit_dst = false;
};

struct OutputSpoutLuid {
    bool valid = false;
    std::array<std::uint8_t, 8> bytes{};
};

// DXGI adapter 0: the adapter Spout is expected to take when it makes its own
// hardware device (CreateDX11device passes a null adapter), not proof of the
// sender's real device. Spout falls back to WARP and to the reference driver,
// so VO-5b must compare with the LUID of Spout's actual ID3D11Device. The
// receiver's adapter isn't known at all.
struct OutputSpoutAdapterQuery {
    bool ok = false;
    std::array<std::uint8_t, 8> luid{};
    std::string name;
    std::string error;
};

class OutputSpoutGpuCapabilitySource {
  public:
    virtual ~OutputSpoutGpuCapabilitySource() = default;
    virtual std::vector<std::string> enabledDeviceExtensions() const = 0;
    virtual OutputSpoutImportQuery queryImport(OutputSpoutHandleKind kind) const = 0;
    virtual OutputSpoutFormatQuery queryFormat() const = 0;
    virtual bool timelineSemaphoreEnabled() const = 0;
    virtual bool semaphoreImportable() const = 0;
    virtual OutputSpoutLuid deviceLuid() const = 0;
    virtual OutputSpoutAdapterQuery defaultAdapter() const = 0;
};

struct OutputSpoutGpuPathContext {
    bool windows = false;
    bool spout_built = false;
    bool armed = false;
};

struct OutputSpoutGpuPathFacts {
    bool external_memory_enabled = false;
    bool external_semaphore_enabled = false;
    bool keyed_mutex_enabled = false;
    OutputSpoutImportQuery nt;
    OutputSpoutImportQuery kmt;
    OutputSpoutFormatQuery format;
    // Static prerequisites, not verified synchronization. keyed_mutex is the
    // extension being enabled, and says nothing about a texture: a keyed path
    // needs a bridge made with a keyed mutex, and acquire and release on both
    // APIs. external_semaphore is the extension, the timeline feature and an
    // importable D3D11 fence handle; working D3D11 fences aren't checked.
    bool sync_keyed_mutex = false;
    bool sync_external_semaphore = false;
    OutputSpoutLuid device_luid;
    OutputSpoutAdapterQuery adapter;
};

// state: available | unavailable. reason: none, or the first that applies of
//   not_checked                  the default, before OutputSinks exists
//   platform_unsupported         not Windows
//   build_without_spout          Windows, PELICAN_WITH_SPOUT off
//   not_requested                no --output-spout
//   external_memory_not_enabled  VK_KHR_external_memory_win32 isn't enabled
//   device_luid_invalid          deviceLUIDValid false
//   dxgi_unavailable             the default adapter can't be read
//   luid_mismatch                incompatible with the current default sender
//                                configuration; it doesn't mean this machine has
//                                no usable GPU
//   capability_query_failed      no handle is importable and a query failed
//                                unexpectedly; detail has the VkResult
//   format_not_importable        neither NT nor KMT says importable
//   format_no_transfer_dst       optimal tiling lacks TRANSFER_DST
//   no_usable_sync               neither way in sync_* is there
//
// Unavailable isn't a Problem and changes nothing: VO-4's activation and its
// failures stay what they are, including a build without Spout, where there is
// no CPU path to fall back to either.
struct OutputSpoutGpuPathReport {
    std::string state = "unavailable";
    std::string reason = "not_checked";
    std::string detail;
    // Absent when the source was never called.
    std::optional<OutputSpoutGpuPathFacts> facts;
};

OutputSpoutGpuPathReport evaluateOutputSpoutGpuPath(
    const OutputSpoutGpuPathContext &context, const OutputSpoutGpuCapabilitySource *source);

nlohmann::json outputSpoutGpuPathJson(const OutputSpoutGpuPathReport &report);

// A snapshot of this device. OutputSinks takes it once, and doesn't outlive the
// VulkanManageCore it asked; --output-spout doesn't change during a run.
OutputSpoutGpuPathReport outputSpoutGpuPathForRun(bool armed, const VulkanManageCore &vulkan);

// VkExternalMemoryHandleTypeFlagBits of a handle kind.
std::uint32_t outputSpoutHandleTypeBit(OutputSpoutHandleKind kind);

struct OutputSpoutImportFeatures {
    bool importable = false;
    bool dedicated_only = false;
};
// From VkExternalMemoryFeatureFlags.
OutputSpoutImportFeatures outputSpoutImportFeatures(std::uint32_t features);

} // namespace Pelican
