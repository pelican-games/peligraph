// VO-5a: the only source of the GPU path check that sees Windows or DXGI headers.
// It reads and asks; it makes no D3D11 device and imports nothing.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dxgi.h>

#include "outputspoutgpusource.hpp"

#include <cstring>
#include <utility>

namespace Pelican {
namespace {

std::string vkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    default: return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
    }
}

std::string narrow(const wchar_t *text) {
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

class RealSource final : public OutputSpoutGpuCapabilitySource {
  public:
    RealSource(VkPhysicalDevice physical_device, std::vector<std::string> enabled, bool timeline,
               OutputSpoutVulkanQueries queries)
        : physical_device_(physical_device),
          enabled_(std::move(enabled)),
          timeline_(timeline),
          queries_(queries) {}

    std::vector<std::string> enabledDeviceExtensions() const override { return enabled_; }

    OutputSpoutImportQuery queryImport(OutputSpoutHandleKind kind) const override {
        VkPhysicalDeviceExternalImageFormatInfo external_info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external_info.handleType =
            static_cast<VkExternalMemoryHandleTypeFlagBits>(outputSpoutHandleTypeBit(kind));
        VkPhysicalDeviceImageFormatInfo2 info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext = &external_info;
        info.format = VK_FORMAT_B8G8R8A8_UNORM;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkExternalImageFormatProperties external_properties{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        properties.pNext = &external_properties;

        const auto result = queries_.image_format(physical_device_, &info, &properties);
        OutputSpoutImportQuery query;
        query.result = vkResultName(result);
        if (result != VK_SUCCESS) {
            query.query_failed = result != VK_ERROR_FORMAT_NOT_SUPPORTED;
            return query;
        }
        const auto features = outputSpoutImportFeatures(
            external_properties.externalMemoryProperties.externalMemoryFeatures);
        query.importable = features.importable;
        query.dedicated_only = features.dedicated_only;
        query.max_width = properties.imageFormatProperties.maxExtent.width;
        query.max_height = properties.imageFormatProperties.maxExtent.height;
        return query;
    }

    OutputSpoutFormatQuery queryFormat() const override {
        VkFormatProperties properties{};
        queries_.format(physical_device_, VK_FORMAT_B8G8R8A8_UNORM, &properties);
        return {(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0,
                (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0};
    }

    bool timelineSemaphoreEnabled() const override { return timeline_; }

    bool semaphoreImportable() const override {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkPhysicalDeviceExternalSemaphoreInfo info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        info.pNext = &type;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        VkExternalSemaphoreProperties properties{
            VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        queries_.semaphore(physical_device_, &info, &properties);
        return (properties.externalSemaphoreFeatures &
                VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
    }

    OutputSpoutLuid deviceLuid() const override {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &id;
        queries_.properties(physical_device_, &properties);
        OutputSpoutLuid luid;
        luid.valid = id.deviceLUIDValid == VK_TRUE;
        std::memcpy(luid.bytes.data(), id.deviceLUID, luid.bytes.size());
        return luid;
    }

    OutputSpoutAdapterQuery defaultAdapter() const override {
        OutputSpoutAdapterQuery query;
        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
            query.error = "CreateDXGIFactory1 failed";
            return query;
        }
        IDXGIAdapter1 *adapter = nullptr;
        if (FAILED(factory->EnumAdapters1(0, &adapter))) {
            query.error = "DXGI lists no adapter";
        } else {
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description))) {
                query.error = "IDXGIAdapter1::GetDesc1 failed";
            } else {
                static_assert(sizeof(description.AdapterLuid) == query.luid.size());
                std::memcpy(query.luid.data(), &description.AdapterLuid, query.luid.size());
                query.name = narrow(description.Description);
                query.ok = true;
            }
            adapter->Release();
        }
        factory->Release();
        return query;
    }

  private:
    VkPhysicalDevice physical_device_;
    std::vector<std::string> enabled_;
    bool timeline_;
    OutputSpoutVulkanQueries queries_;
};

} // namespace

std::unique_ptr<OutputSpoutGpuCapabilitySource> makeOutputSpoutGpuCapabilitySource(
    VkPhysicalDevice physical_device, std::vector<std::string> enabled, bool timeline,
    OutputSpoutVulkanQueries queries) {
    return std::make_unique<RealSource>(physical_device, std::move(enabled), timeline, queries);
}

} // namespace Pelican
