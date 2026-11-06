#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

// VO-5a: who wants which Vulkan extension, written down before the device
// exists. A plain value, local to VulkanManageCore's constructor: there is no
// global table, so nothing can register late or by static initialization order.
enum class VulkanExtensionNeed : std::uint8_t { optional, required };

struct VulkanExtensionRequest {
    std::string name;
    VulkanExtensionNeed need = VulkanExtensionNeed::optional;
    std::vector<std::string> requested_by;
};

// Two requests for one name merge into one row: it keeps the first position,
// required wins over optional, and every requester is listed once. That's the
// only conflict there is, since nothing can ask for an extension to be absent.
class VulkanExtensionRequests {
  public:
    void requestInstance(std::string name, VulkanExtensionNeed need, std::string requested_by);
    void requestDevice(std::string name, VulkanExtensionNeed need, std::string requested_by);
    std::span<const VulkanExtensionRequest> instance() const { return instance_; }
    std::span<const VulkanExtensionRequest> device() const { return device_; }

  private:
    std::vector<VulkanExtensionRequest> instance_;
    std::vector<VulkanExtensionRequest> device_;
};

enum class VulkanExtensionLevel : std::uint8_t { instance, device };

struct VulkanExtensionRow {
    enum class Outcome : std::uint8_t { enabled, skipped_unsupported, refused_unsupported };
    std::string name;
    VulkanExtensionNeed need = VulkanExtensionNeed::optional;
    std::vector<std::string> requested_by;
    Outcome outcome = Outcome::enabled;
};

struct VulkanExtensionResolution {
    std::vector<std::string> enabled;
    std::vector<VulkanExtensionRow> rows;
    // vulkan.required_extension_missing@1, for the first refused row. The
    // caller throws it, in the error type of its bootstrap.
    std::optional<std::string> refusal;
};

// `enabled` is `base` in its order, followed by each supported row in row order
// unless the name is already there. An unsupported optional row is skipped and
// reported; an unsupported required one refuses by name.
VulkanExtensionResolution resolveVulkanExtensions(
    VulkanExtensionLevel level,
    std::vector<std::string> base,
    std::span<const VulkanExtensionRequest> requests,
    std::span<const std::string> supported);

// XR's instance list as required rows requested by "openxr", first and in its
// own order, then the table's rows. The table stays as it is: the flat retry
// after a failed XR attempt gets it without any "openxr" row.
VulkanExtensionRequests withOpenXrInstanceRequests(
    const VulkanExtensionRequests &requests, std::span<const std::string> names);

// The one registration call. Called once, before the instance and the device
// are created; the rows come from the feature's own call and from nowhere else.
VulkanExtensionRequests collectVulkanExtensionRequests(bool output_spout);

struct VulkanExtensionTableReport {
    std::vector<std::string> enabled;
    std::vector<VulkanExtensionRow> rows;
};

// `enabled` is what the engine asked for, passed in the create info. When an
// OpenXR runtime created the device it may have added extensions the engine
// can't see, so with device_created_by "openxr" this isn't a complete list.
struct VulkanExtensionReport {
    VulkanExtensionTableReport instance;
    VulkanExtensionTableReport device;
    std::string device_created_by = "engine";
};

nlohmann::json vulkanExtensionReportJson(const VulkanExtensionReport &report);

#if PELICAN_OUTPUT_TEST_SEAMS
// Rows a test adds to the table, merged after collectVulkanExtensionRequests.
void setExtraVulkanExtensionRequestsForTesting(
    std::optional<VulkanExtensionRequests> requests);
void applyExtraVulkanExtensionRequestsForTesting(VulkanExtensionRequests &requests);
// Every create attempt of this process, in order, read from the create info
// right before the create call, on both the flat and the XR path.
struct VulkanCreationRecord {
    std::vector<std::vector<std::string>> instance;
    std::vector<std::vector<std::string>> device;
};
VulkanCreationRecord &vulkanCreationRecordForTesting();
void recordVulkanCreationForTesting(VulkanExtensionLevel level, std::uint32_t count,
                                    const char *const *names);
#endif

} // namespace Pelican
