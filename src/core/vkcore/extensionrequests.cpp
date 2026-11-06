#include "extensionrequests.hpp"

#if PELICAN_WITH_SPOUT
#include "../output/outputspoutgpupath.hpp"
#endif

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

void addRequest(std::vector<VulkanExtensionRequest> &rows, std::string name,
                VulkanExtensionNeed need, std::string requested_by) {
    if (name.empty()) throw std::logic_error("a Vulkan extension request has an empty name");
    const auto row = std::find_if(rows.begin(), rows.end(),
                                  [&](const auto &entry) { return entry.name == name; });
    if (row == rows.end()) {
        rows.push_back({std::move(name), need, {std::move(requested_by)}});
        return;
    }
    if (need == VulkanExtensionNeed::required) row->need = need;
    if (std::find(row->requested_by.begin(), row->requested_by.end(), requested_by) ==
        row->requested_by.end()) {
        row->requested_by.push_back(std::move(requested_by));
    }
}

std::string joined(const std::vector<std::string> &values) {
    std::string result;
    for (const auto &value : values) {
        if (!result.empty()) result += ", ";
        result += value;
    }
    return result;
}

const char *needName(VulkanExtensionNeed need) {
    return need == VulkanExtensionNeed::required ? "required" : "optional";
}

const char *outcomeName(VulkanExtensionRow::Outcome outcome) {
    switch (outcome) {
    case VulkanExtensionRow::Outcome::enabled: return "enabled";
    case VulkanExtensionRow::Outcome::skipped_unsupported: return "skipped_unsupported";
    case VulkanExtensionRow::Outcome::refused_unsupported: return "refused_unsupported";
    }
    return "enabled";
}

nlohmann::json tableJson(const VulkanExtensionTableReport &table) {
    auto rows = nlohmann::json::array();
    for (const auto &row : table.rows) {
        rows.push_back({{"name", row.name},
                        {"need", needName(row.need)},
                        {"requested_by", row.requested_by},
                        {"outcome", outcomeName(row.outcome)}});
    }
    return {{"enabled", table.enabled}, {"rows", std::move(rows)}};
}

} // namespace

void VulkanExtensionRequests::requestInstance(std::string name, VulkanExtensionNeed need,
                                              std::string requested_by) {
    addRequest(instance_, std::move(name), need, std::move(requested_by));
}

void VulkanExtensionRequests::requestDevice(std::string name, VulkanExtensionNeed need,
                                            std::string requested_by) {
    addRequest(device_, std::move(name), need, std::move(requested_by));
}

VulkanExtensionResolution resolveVulkanExtensions(
    VulkanExtensionLevel level,
    std::vector<std::string> base,
    std::span<const VulkanExtensionRequest> requests,
    std::span<const std::string> supported) {
    VulkanExtensionResolution result;
    result.enabled = std::move(base);
    for (const auto &request : requests) {
        VulkanExtensionRow row{request.name, request.need, request.requested_by};
        const auto in_base =
            std::find(result.enabled.begin(), result.enabled.end(), request.name) !=
            result.enabled.end();
        const auto is_supported =
            std::find(supported.begin(), supported.end(), request.name) != supported.end();
        if (in_base) {
            row.outcome = VulkanExtensionRow::Outcome::enabled;
        } else if (is_supported) {
            row.outcome = VulkanExtensionRow::Outcome::enabled;
            result.enabled.push_back(request.name);
        } else if (request.need == VulkanExtensionNeed::required) {
            row.outcome = VulkanExtensionRow::Outcome::refused_unsupported;
            if (!result.refusal) {
                result.refusal =
                    std::string{"vulkan.required_extension_missing@1: "} +
                    (level == VulkanExtensionLevel::instance ? "instance" : "device") +
                    " extension " + request.name + ", requested by " +
                    joined(request.requested_by);
            }
        } else {
            row.outcome = VulkanExtensionRow::Outcome::skipped_unsupported;
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}

VulkanExtensionRequests withOpenXrInstanceRequests(
    const VulkanExtensionRequests &requests, std::span<const std::string> names) {
    VulkanExtensionRequests merged;
    for (const auto &name : names) {
        merged.requestInstance(name, VulkanExtensionNeed::required, "openxr");
    }
    for (const auto &row : requests.instance()) {
        for (const auto &requester : row.requested_by) {
            merged.requestInstance(row.name, row.need, requester);
        }
    }
    for (const auto &row : requests.device()) {
        for (const auto &requester : row.requested_by) {
            merged.requestDevice(row.name, row.need, requester);
        }
    }
    return merged;
}

VulkanExtensionRequests collectVulkanExtensionRequests(bool output_spout) {
    VulkanExtensionRequests requests;
#if PELICAN_WITH_SPOUT
    registerOutputSpoutExtensionRequests(output_spout, requests);
#else
    (void)output_spout;
#endif
    return requests;
}

nlohmann::json vulkanExtensionReportJson(const VulkanExtensionReport &report) {
    return {{"list_scope", "engine_requested"},
            {"device_created_by", report.device_created_by},
            {"instance", tableJson(report.instance)},
            {"device", tableJson(report.device)}};
}

#if PELICAN_OUTPUT_TEST_SEAMS
namespace {
std::optional<VulkanExtensionRequests> &extraRequests() {
    static std::optional<VulkanExtensionRequests> requests;
    return requests;
}
} // namespace

void setExtraVulkanExtensionRequestsForTesting(
    std::optional<VulkanExtensionRequests> requests) {
    extraRequests() = std::move(requests);
}

void applyExtraVulkanExtensionRequestsForTesting(VulkanExtensionRequests &requests) {
    if (!extraRequests()) return;
    for (const auto &row : extraRequests()->instance()) {
        for (const auto &requester : row.requested_by) {
            requests.requestInstance(row.name, row.need, requester);
        }
    }
    for (const auto &row : extraRequests()->device()) {
        for (const auto &requester : row.requested_by) {
            requests.requestDevice(row.name, row.need, requester);
        }
    }
}

VulkanCreationRecord &vulkanCreationRecordForTesting() {
    static VulkanCreationRecord record;
    return record;
}

void recordVulkanCreationForTesting(VulkanExtensionLevel level, std::uint32_t count,
                                    const char *const *names) {
    std::vector<std::string> list;
    for (std::uint32_t index = 0; index < count; ++index) list.emplace_back(names[index]);
    auto &record = vulkanCreationRecordForTesting();
    (level == VulkanExtensionLevel::instance ? record.instance : record.device)
        .push_back(std::move(list));
}
#endif

} // namespace Pelican
