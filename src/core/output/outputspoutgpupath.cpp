#include "outputspoutgpupath.hpp"

#include "../log.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/extensionrequests.hpp"
#if PELICAN_WITH_SPOUT
#include "outputspoutgpusource.hpp"
#endif

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <span>
#include <utility>

namespace Pelican {
namespace {

OutputSpoutGpuPathReport unavailable(std::string reason, std::string detail) {
    OutputSpoutGpuPathReport report;
    report.reason = std::move(reason);
    report.detail = std::move(detail);
    return report;
}

std::string luidText(std::span<const std::uint8_t, 8> bytes) {
    std::string text;
    char digits[3];
    for (const auto byte : bytes) {
        std::snprintf(digits, sizeof(digits), "%02x", static_cast<unsigned>(byte));
        text += digits;
    }
    return text;
}

nlohmann::json importJson(const OutputSpoutImportQuery &query) {
    return {{"result", query.result},
            {"importable", query.importable},
            {"dedicated_only", query.dedicated_only},
            {"max_width", query.max_width},
            {"max_height", query.max_height}};
}

} // namespace

void registerOutputSpoutExtensionRequests(bool output_spout,
                                          VulkanExtensionRequests &requests) {
    if (!output_spout) return;
    for (const auto *name : {output_spout_external_memory_extension,
                             output_spout_external_semaphore_extension,
                             output_spout_keyed_mutex_extension}) {
        requests.requestDevice(name, VulkanExtensionNeed::optional, "output.spout");
    }
}

OutputSpoutGpuPathReport evaluateOutputSpoutGpuPath(
    const OutputSpoutGpuPathContext &context, const OutputSpoutGpuCapabilitySource *source) {
    if (!context.windows) {
        return unavailable("platform_unsupported", "Spout exists only on Windows");
    }
    if (!context.spout_built) {
        return unavailable("build_without_spout", "this build has no Spout");
    }
    if (!context.armed) {
        return unavailable("not_requested", "the player wasn't given --output-spout");
    }
    if (source == nullptr) return {};

    // Every fact first, so an unavailable report still shows them all.
    OutputSpoutGpuPathFacts facts;
    const auto enabled = source->enabledDeviceExtensions();
    const auto is_enabled = [&](const char *name) {
        return std::find(enabled.begin(), enabled.end(), name) != enabled.end();
    };
    facts.external_memory_enabled = is_enabled(output_spout_external_memory_extension);
    facts.external_semaphore_enabled = is_enabled(output_spout_external_semaphore_extension);
    facts.keyed_mutex_enabled = is_enabled(output_spout_keyed_mutex_extension);
    facts.nt = source->queryImport(OutputSpoutHandleKind::nt);
    facts.kmt = source->queryImport(OutputSpoutHandleKind::kmt);
    facts.format = source->queryFormat();
    const auto timeline = source->timelineSemaphoreEnabled();
    const auto semaphore = source->semaphoreImportable();
    facts.sync_keyed_mutex = facts.keyed_mutex_enabled;
    facts.sync_external_semaphore = facts.external_semaphore_enabled && timeline && semaphore;
    facts.device_luid = source->deviceLuid();
    facts.adapter = source->defaultAdapter();

    OutputSpoutGpuPathReport report;
    report.facts = facts;
    const auto refuse = [&](const char *reason, std::string detail) {
        report.reason = reason;
        report.detail = std::move(detail);
        return report;
    };
    if (!facts.external_memory_enabled) {
        return refuse("external_memory_not_enabled",
                      std::string{output_spout_external_memory_extension} +
                          " isn't enabled on this device");
    }
    if (!facts.device_luid.valid) {
        return refuse("device_luid_invalid", "this Vulkan device has no valid LUID");
    }
    if (!facts.adapter.ok) return refuse("dxgi_unavailable", facts.adapter.error);
    if (facts.device_luid.bytes != facts.adapter.luid) {
        return refuse("luid_mismatch",
                      "the Vulkan device isn't on DXGI adapter 0, '" + facts.adapter.name +
                          "', which Spout's default sender is expected to use");
    }
    if (!facts.nt.importable && !facts.kmt.importable) {
        if (facts.nt.query_failed || facts.kmt.query_failed) {
            return refuse("capability_query_failed",
                          "vkGetPhysicalDeviceImageFormatProperties2: NT " + facts.nt.result +
                              ", KMT " + facts.kmt.result);
        }
        return refuse("format_not_importable",
                      "neither a D3D11 NT nor a KMT handle of B8G8R8A8_UNORM is importable");
    }
    if (!facts.format.transfer_dst) {
        return refuse("format_no_transfer_dst",
                      "optimal tiling of B8G8R8A8_UNORM lacks TRANSFER_DST");
    }
    if (!facts.sync_keyed_mutex && !facts.sync_external_semaphore) {
        return refuse("no_usable_sync",
                      "neither VK_KHR_win32_keyed_mutex nor an importable D3D11 fence");
    }
    report.state = "available";
    report.reason = "none";
    return report;
}

nlohmann::json outputSpoutGpuPathJson(const OutputSpoutGpuPathReport &report) {
    nlohmann::json json{{"state", report.state},
                        {"reason", report.reason},
                        {"detail", report.detail},
                        {"extensions", nullptr},
                        {"format", nullptr},
                        {"handles", nullptr},
                        {"sync", nullptr},
                        {"luid", nullptr}};
    if (!report.facts) return json;
    const auto &facts = *report.facts;
    json["extensions"] = {
        {output_spout_external_memory_extension, facts.external_memory_enabled},
        {output_spout_external_semaphore_extension, facts.external_semaphore_enabled},
        {output_spout_keyed_mutex_extension, facts.keyed_mutex_enabled}};
    json["format"] = {{"dxgi", "DXGI_FORMAT_B8G8R8A8_UNORM"},
                      {"vulkan", "VK_FORMAT_B8G8R8A8_UNORM"},
                      {"tiling", "optimal"},
                      {"transfer_dst", facts.format.transfer_dst},
                      {"blit_dst", facts.format.blit_dst}};
    json["handles"] = {{"nt", importJson(facts.nt)}, {"kmt", importJson(facts.kmt)}};
    json["sync"] = {{"keyed_mutex", facts.sync_keyed_mutex},
                    {"external_semaphore", facts.sync_external_semaphore}};
    json["luid"] = {
        {"vulkan_valid", facts.device_luid.valid},
        {"vulkan", luidText(facts.device_luid.bytes)},
        {"dxgi", facts.adapter.ok ? nlohmann::json(luidText(facts.adapter.luid)) : nlohmann::json(nullptr)},
        {"dxgi_adapter", facts.adapter.name}};
    return json;
}

OutputSpoutGpuPathReport outputSpoutGpuPathForRun(bool armed, const VulkanManageCore &vulkan) {
#if defined(_WIN32)
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    const OutputSpoutGpuPathContext context{windows, PELICAN_WITH_SPOUT != 0, armed};
#if PELICAN_WITH_SPOUT
    if (armed) {
        const auto enabled = vulkan.getEnabledDeviceExtensions();
        const auto source = makeOutputSpoutGpuCapabilitySource(
            static_cast<VkPhysicalDevice>(vulkan.getPhysDevice()),
            std::vector<std::string>{enabled.begin(), enabled.end()},
            vulkan.getRuntimeCapabilities().timeline_semaphore);
        return evaluateOutputSpoutGpuPath(context, source.get());
    }
#else
    (void)vulkan;
#endif
    return evaluateOutputSpoutGpuPath(context, nullptr);
}

std::uint32_t outputSpoutHandleTypeBit(OutputSpoutHandleKind kind) {
    return kind == OutputSpoutHandleKind::nt
               ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
               : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
}

OutputSpoutImportFeatures outputSpoutImportFeatures(std::uint32_t features) {
    return {(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0,
            (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0};
}

} // namespace Pelican
