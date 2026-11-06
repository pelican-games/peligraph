// VO-4: the one source that sees Windows, D3D11 or Spout headers. Everything in
// this file runs on one sink's sender thread, except the factory's one call to
// disable logging, which is made before any such thread starts.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SpoutDX.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include "outputspout.hpp"

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Pelican {
namespace {

// SpoutUtils keeps its last log line in unsynchronized process-global state,
// written from every sender and receiver. DisableLogs makes _doLog return first.
void disableSpoutLogsOnce() {
    static std::once_flag once;
    std::call_once(once, [] { spoututils::DisableLogs(); });
}

// The shared texture is protected, and whether it exists after a failed size
// change decides between a passing failure and a terminal one.
class SenderDx final : public spoutDX {
  public:
    bool hasSharedTexture() const { return m_pSharedTexture != nullptr; }
};

using Microsoft::WRL::ComPtr;

std::string hexText(HRESULT value) {
    char text[32];
    std::snprintf(text, sizeof(text), "HRESULT 0x%08lX", static_cast<unsigned long>(value));
    return text;
}

OutputSpoutGpuStep stepFromHresult(HRESULT value, const char *reason, const std::string &what) {
    return {outputSpoutClassifyHresult(static_cast<std::int32_t>(value)), reason,
            what + ": " + hexText(value)};
}

OutputSpoutGpuStep stepFromVk(VkResult value, const char *reason, const std::string &what) {
    return {outputSpoutClassifyVkResult(static_cast<std::int32_t>(value)), reason,
            what + ": VkResult " + std::to_string(static_cast<int>(value))};
}

// A device whose work never finished, and everything its queue may still touch:
// kept for the process's life. See abandonGpu.
struct AbandonedD3D11 {
    std::unique_ptr<SenderDx> sender;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    ComPtr<ID3D11Fence> shared_fence;
    ComPtr<ID3D11Fence> done_fence;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11Device5> device;
    HANDLE event = nullptr;
};

class SpoutDxBackend final : public OutputSpoutBackend, public OutputSpoutGpuBackend {
  public:
    explicit SpoutDxBackend(bool create_device, OutputSpoutGpuRequest request = {})
        : create_device_(create_device), has_gpu_(request.device != VK_NULL_HANDLE) {
#if PELICAN_OUTPUT_TEST_SEAMS
        if (const char *fault = std::getenv("PELICAN_OUTPUT_SPOUT_GPU_FAULT")) fault_ = fault;
#endif
    }
    ~SpoutDxBackend() override { close(); }

    OutputSpoutGpuBackend *gpu() override { return has_gpu_ ? this : nullptr; }

    OutputSpoutGpuStep prepare(const OutputSpoutGpuRequest &request,
                               VkSemaphore &semaphore) override {
        auto step = prepareParts(request, semaphore);
        // Nothing of a failed attempt stays alive: the sender only releases what
        // has been prepared.
        if (step.failure != OutputSpoutGpuFailure::none) releaseGpu();
        return step;
    }

    OutputSpoutGpuStep createBridges(std::uint32_t width, std::uint32_t height,
                                     std::array<VkImage, output_spout_bridge_count> &images) override {
        ++create_calls_;
        for (std::size_t index = 0; index < images.size(); ++index) {
            auto step = createBridge(index, width, height);
            if (step.failure != OutputSpoutGpuFailure::none) {
                destroyBridges();
                return step;
            }
            images[index] = bridges_[index].image;
        }
        return {};
    }

    OutputSpoutGpuStep prepareParts(const OutputSpoutGpuRequest &request,
                                    VkSemaphore &semaphore) {
        device_ = request.device;
        import_semaphore_ = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(device_, "vkImportSemaphoreWin32HandleKHR"));
        handle_properties_ = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetMemoryWin32HandlePropertiesKHR"));
        if (import_semaphore_ == nullptr || handle_properties_ == nullptr) {
            return {OutputSpoutGpuFailure::permanent, "semaphore_import_failed",
                    "the Win32 external entry points aren't on this device"};
        }
        ComPtr<IDXGIDevice> dxgi;
        auto *d3d = sender_->GetDX11Device();
        auto result = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (FAILED(result)) return stepFromHresult(result, "sender_device_mismatch", "IDXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        result = dxgi->GetAdapter(&adapter);
        if (FAILED(result)) return stepFromHresult(result, "sender_device_mismatch", "GetAdapter");
        DXGI_ADAPTER_DESC description{};
        result = adapter->GetDesc(&description);
        if (FAILED(result)) return stepFromHresult(result, "sender_device_mismatch", "GetDesc");
        std::array<std::uint8_t, 8> luid{};
        std::memcpy(luid.data(), &description.AdapterLuid, luid.size());
        if (luid != request.vulkan_luid || faultIs("device_luid")) {
            return {OutputSpoutGpuFailure::permanent, "sender_device_mismatch",
                    "Spout's D3D11 device isn't on the Vulkan device's adapter"};
        }
        if (faultIs("d3d11_fence")) {
            return stepFromHresult(E_NOINTERFACE, "d3d11_fence_unavailable", "injected");
        }
        result = d3d->QueryInterface(IID_PPV_ARGS(&device5_));
        if (FAILED(result)) return stepFromHresult(result, "d3d11_fence_unavailable", "ID3D11Device5");
        result = sender_->GetDX11Context()->QueryInterface(IID_PPV_ARGS(&context4_));
        if (FAILED(result))
            return stepFromHresult(result, "d3d11_fence_unavailable", "ID3D11DeviceContext4");
        result = device5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared_fence_));
        if (FAILED(result)) return stepFromHresult(result, "d3d11_fence_unavailable", "shared fence");
        result = device5_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_fence_));
        if (FAILED(result)) return stepFromHresult(result, "d3d11_fence_unavailable", "local fence");
        HANDLE fence_handle = nullptr;
        result = shared_fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
        if (FAILED(result))
            return stepFromHresult(result, "d3d11_fence_unavailable", "CreateSharedHandle");

        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        create.pNext = &type;
        auto vk_result = vkCreateSemaphore(device_, &create, nullptr, &semaphore_);
        if (vk_result != VK_SUCCESS) {
            CloseHandle(fence_handle);
            return stepFromVk(vk_result, "semaphore_import_failed", "vkCreateSemaphore");
        }
        VkImportSemaphoreWin32HandleInfoKHR import{
            VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        import.semaphore = semaphore_;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        import.handle = fence_handle;
        vk_result = faultIs("semaphore_import") ? VK_ERROR_INVALID_EXTERNAL_HANDLE
                                                : import_semaphore_(device_, &import);
        // Importing doesn't take the handle's ownership for this type.
        CloseHandle(fence_handle);
        if (vk_result != VK_SUCCESS)
            return stepFromVk(vk_result, "semaphore_import_failed", "the fence's import");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_ == nullptr) {
            return {OutputSpoutGpuFailure::transient, "d3d11_fence_unavailable", "CreateEvent"};
        }
        done_value_ = 0;
        pending_done_ = 0;
        semaphore = semaphore_;
        return {};
    }

    OutputSpoutSentBridge sendBridge(std::size_t bridge, std::uint64_t wait_value) override {
#if PELICAN_OUTPUT_TEST_SEAMS
        if (faultIs("block_send")) {
            outputSpoutGpuSendBlockedForTesting() = true;
            while (outputSpoutGpuBlockSendForTesting())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            outputSpoutGpuSendBlockedForTesting() = false;
        }
#endif
        // A real wait that no signal will ever reach, and the copy queued behind it.
        const auto value = faultIs("wait_forever") ? wait_value + (1ull << 40) : wait_value;
        auto result = context4_->Wait(shared_fence_.Get(), value);
        if (FAILED(result)) {
            const auto kind = outputSpoutClassifyHresult(static_cast<std::int32_t>(result)) ==
                                      OutputSpoutGpuFailure::device_removed
                                  ? OutputSpoutResultKind::device_removed
                                  : OutputSpoutResultKind::failed;
            return {{kind, "ID3D11DeviceContext4::Wait: " + hexText(result)}, false};
        }
        OutputSpoutResult sent;
        const auto accepted = sender_->SendTexture(bridges_[bridge].texture.Get()) &&
                              !faultIs("send_false");
        auto *device = sender_->GetDX11Device();
        const auto removed = device != nullptr ? device->GetDeviceRemovedReason() : S_OK;
        if (removed != S_OK || faultIs("device_removed")) {
            // Nothing queued on a removed device can be waited for.
            return {{OutputSpoutResultKind::device_removed,
                     "device removed, " + hexText(removed)}, false};
        }
        if (!accepted) {
            sent = {OutputSpoutResultKind::failed, "SendTexture returned false"};
        } else if (!published_) {
            if (sender_->IsInitialized() &&
                sender_->sendernames.FindSenderName(actual_.c_str())) {
                published_ = true;
            } else {
                sent = {OutputSpoutResultKind::failed,
                        "'" + actual_ + "' isn't in Spout's sender list"};
            }
        }
        // Even after a failed send: the wait is queued, so completion is tracked.
        pending_done_ = ++done_value_;
        result = faultIs("signal_fail") ? E_FAIL
                                        : context4_->Signal(done_fence_.Get(), pending_done_);
        sender_->GetDX11Context()->Flush();
        if (FAILED(result) && sent.kind == OutputSpoutResultKind::ok) {
            sent = {OutputSpoutResultKind::failed,
                    "ID3D11DeviceContext4::Signal: " + hexText(result)};
        }
        return {sent, true};
    }

    OutputSpoutSentState waitSent(std::chrono::milliseconds limit) override {
        if (faultOnce("sync_late", late_fired_)) return OutputSpoutSentState::timeout;
        if (faultOnce("event_fail", event_fail_fired_)) return OutputSpoutSentState::failed;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (true) {
            const auto completed = done_fence_->GetCompletedValue();
            if (completed == UINT64_MAX) return OutputSpoutSentState::device_removed;
            if (completed >= pending_done_) return OutputSpoutSentState::done;
            const auto removed = sender_->GetDX11Device()->GetDeviceRemovedReason();
            if (removed != S_OK) return OutputSpoutSentState::device_removed;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return OutputSpoutSentState::timeout;
            const auto armed = done_fence_->SetEventOnCompletion(pending_done_, event_);
            if (FAILED(armed)) {
                return outputSpoutClassifyHresult(static_cast<std::int32_t>(armed)) ==
                               OutputSpoutGpuFailure::device_removed
                           ? OutputSpoutSentState::device_removed
                           : OutputSpoutSentState::failed;
            }
            const auto wait = WaitForSingleObject(
                event_, static_cast<DWORD>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
            if (wait == WAIT_FAILED) return OutputSpoutSentState::failed;
            // A timeout or an old signal: the top of the loop reads the real value.
        }
    }

    void destroyBridges() override {
        for (auto &bridge : bridges_) {
            if (bridge.image != VK_NULL_HANDLE) vkDestroyImage(device_, bridge.image, nullptr);
            if (bridge.memory != VK_NULL_HANDLE) vkFreeMemory(device_, bridge.memory, nullptr);
            bridge = {};
        }
    }

    void releaseGpu() override {
        if (semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore_, nullptr);
        semaphore_ = VK_NULL_HANDLE;
        done_fence_.Reset();
        shared_fence_.Reset();
        context4_.Reset();
        device5_.Reset();
        if (event_ != nullptr) CloseHandle(event_);
        event_ = nullptr;
    }

    void abandonGpu() override {
        auto *kept = new AbandonedD3D11;
        for (auto &bridge : bridges_) {
            if (bridge.image != VK_NULL_HANDLE) vkDestroyImage(device_, bridge.image, nullptr);
            if (bridge.memory != VK_NULL_HANDLE) vkFreeMemory(device_, bridge.memory, nullptr);
            kept->textures.push_back(std::move(bridge.texture));
            bridge = {};
        }
        if (semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore_, nullptr);
        semaphore_ = VK_NULL_HANDLE;
        kept->shared_fence = std::move(shared_fence_);
        kept->done_fence = std::move(done_fence_);
        kept->context = std::move(context4_);
        kept->device = std::move(device5_);
        kept->event = event_;
        event_ = nullptr;
        kept->sender = std::move(sender_);
    }

    OutputSpoutResult open(const std::string &name) override {
        sender_ = std::make_unique<SenderDx>();
        published_ = false;
#if PELICAN_OUTPUT_TEST_SEAMS
        if (faultIs("warp_device")) {
            // A real WARP device, as Spout gets when hardware creation fails.
            ID3D11Device *warp = nullptr;
            ID3D11DeviceContext *context = nullptr;
            const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
            if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                                            D3D11_SDK_VERSION, &warp, nullptr, &context))) {
                context->Release();
                (void)sender_->OpenDirectX11(warp);
                warp_device_.Attach(warp);
            }
        } else
#endif
        // OpenDirectX11 returns true whether or not it made a device (SpoutDX.cpp:215).
        if (create_device_) (void)sender_->OpenDirectX11();
        if (sender_->GetDX11Device() == nullptr || sender_->GetDX11Context() == nullptr)
            return {OutputSpoutResultKind::open_failed, "no D3D11 device"};
        // RegisterSenderName skips the registration past the limit and still
        // returns true (SpoutSenderNames.cpp:168), so the limit is checked first.
        const auto count = sender_->GetSenderCount();
        const auto limit = sender_->GetMaxSenders();
        if (count >= limit) {
            return {OutputSpoutResultKind::open_failed,
                    "sender limit " + std::to_string(limit) + " reached"};
        }
        // Appends _1, _2... when the name is registered (SpoutDX.cpp:328); the
        // sender compares actualName() with what it asked for. Nothing is
        // published yet: that's the first SendImage.
        sender_->SetSenderName(name.c_str());
        actual_ = sender_->GetName();
        return {};
    }

    OutputSpoutResult send(const std::uint8_t *bgra, std::uint32_t width,
                           std::uint32_t height) override {
        // No pitch in this tag: UpdateSubresource uses width * 4 and no flip.
        const auto accepted =
            sender_->SendImage(const_cast<unsigned char *>(bgra), width, height);
        if (auto *device = sender_->GetDX11Device()) {
            const auto reason = device->GetDeviceRemovedReason();
            if (reason != S_OK) {
                char text[64];
                std::snprintf(text, sizeof(text), "device removed, HRESULT 0x%08lX",
                              static_cast<unsigned long>(reason));
                return {OutputSpoutResultKind::device_removed, text};
            }
        }
        if (!accepted) {
            if (!published_ || !sender_->hasSharedTexture()) {
                return {OutputSpoutResultKind::open_failed,
                        "no shared texture or sender for " + std::to_string(width) +
                            "x" + std::to_string(height)};
            }
            return {OutputSpoutResultKind::failed, "SendImage returned false"};
        }
        if (!published_) {
            if (!sender_->IsInitialized() ||
                !sender_->sendernames.FindSenderName(actual_.c_str())) {
                return {OutputSpoutResultKind::open_failed,
                        "'" + actual_ + "' isn't in Spout's sender list"};
            }
            published_ = true;
        }
        return {};
    }

    std::string actualName() const override { return actual_; }

    void close() override {
        if (!sender_) return;
        sender_->ReleaseSender();
        sender_->CloseDirectX11();
        sender_.reset();
        published_ = false;
    }

  private:
    struct Bridge {
        ComPtr<ID3D11Texture2D> texture;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    bool faultIs(std::string_view name) const { return fault_ == name; }
    bool faultOnce(std::string_view name, bool &fired) const {
        if (!faultIs(name) || fired) return false;
        fired = true;
        return true;
    }

    OutputSpoutGpuStep createBridge(std::size_t index, std::uint32_t width, std::uint32_t height) {
        auto &bridge = bridges_[index];
        if (faultIs("oom_bridge") || (faultIs("size_remake") && create_calls_ > 1)) {
            return stepFromHresult(E_OUTOFMEMORY, "bridge_create_failed", "injected");
        }
        if (faultIs("bridge_create_invalid") ||
            (faultIs("bridge_create_second") && index == 1)) {
            return stepFromHresult(E_INVALIDARG, "bridge_create_failed", "injected");
        }
        D3D11_TEXTURE2D_DESC wanted{};
        wanted.Width = width;
        wanted.Height = height;
        wanted.MipLevels = 1;
        wanted.ArraySize = 1;
        wanted.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        wanted.SampleDesc.Count = 1;
        wanted.Usage = D3D11_USAGE_DEFAULT;
        wanted.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        wanted.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        auto result = sender_->GetDX11Device()->CreateTexture2D(&wanted, nullptr, &bridge.texture);
        if (FAILED(result)) return stepFromHresult(result, "bridge_create_failed", "CreateTexture2D");
        D3D11_TEXTURE2D_DESC real{};
        bridge.texture->GetDesc(&real);
        if (real.Width != width || real.Height != height || real.Format != wanted.Format ||
            real.MipLevels != 1 || real.ArraySize != 1 || real.SampleDesc.Count != 1 ||
            (real.MiscFlags & D3D11_RESOURCE_MISC_SHARED) == 0) {
            return {OutputSpoutGpuFailure::permanent, "bridge_create_failed",
                    "the texture D3D11 made isn't the one asked for"};
        }
        ComPtr<IDXGIResource> resource;
        result = bridge.texture.As(&resource);
        if (FAILED(result)) return stepFromHresult(result, "bridge_create_failed", "IDXGIResource");
        HANDLE handle = nullptr;
        result = resource->GetSharedHandle(&handle);
        if (FAILED(result)) return stepFromHresult(result, "bridge_create_failed", "GetSharedHandle");

        constexpr auto handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
        VkExternalMemoryImageCreateInfo external{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = handle_type;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.pNext = &external;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_B8G8R8A8_UNORM;
        image.extent = {real.Width, real.Height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto vk_result = vkCreateImage(device_, &image, nullptr, &bridge.image);
        if (vk_result != VK_SUCCESS)
            return stepFromVk(vk_result, "bridge_import_failed", "vkCreateImage");

        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        VkImageMemoryRequirementsInfo2 requirements_info{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        requirements_info.image = bridge.image;
        vkGetImageMemoryRequirements2(device_, &requirements_info, &requirements);

        VkMemoryWin32HandlePropertiesKHR properties{
            VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        vk_result = handle_properties_(device_, handle_type, handle, &properties);
        if (vk_result != VK_SUCCESS)
            return stepFromVk(vk_result, "bridge_import_failed", "the handle's properties");
        const auto bits = faultIs("memory_types")
                              ? 0u
                              : requirements.memoryRequirements.memoryTypeBits &
                                    properties.memoryTypeBits;
        if (bits == 0) {
            return {OutputSpoutGpuFailure::permanent, "memory_type_mismatch",
                    "the image and the handle share no memory type"};
        }
        std::uint32_t memory_type = 0;
        while ((bits & (1u << memory_type)) == 0) ++memory_type;

        VkMemoryDedicatedAllocateInfo dedicated_allocate{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated_allocate.image = bridge.image;
        VkImportMemoryWin32HandleInfoKHR import{
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        import.pNext = &dedicated_allocate;
        import.handleType = handle_type;
        import.handle = handle;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &import;
        allocate.allocationSize = requirements.memoryRequirements.size;
        allocate.memoryTypeIndex = memory_type;
        if (faultIs("bridge_import_oom")) {
            return stepFromVk(VK_ERROR_OUT_OF_DEVICE_MEMORY, "bridge_out_of_memory", "injected");
        }
        if (faultIs("bridge_import_invalid") ||
            (faultIs("bridge_import_second") && index == 1)) {
            return stepFromVk(VK_ERROR_INVALID_EXTERNAL_HANDLE, "bridge_import_failed", "injected");
        }
        vk_result = vkAllocateMemory(device_, &allocate, nullptr, &bridge.memory);
        if (vk_result != VK_SUCCESS) {
            const auto out_of_memory =
                outputSpoutClassifyVkResult(vk_result) == OutputSpoutGpuFailure::transient;
            return stepFromVk(vk_result,
                              out_of_memory ? "bridge_out_of_memory" : "bridge_import_failed",
                              "vkAllocateMemory");
        }
        vk_result = vkBindImageMemory(device_, bridge.image, bridge.memory, 0);
        if (vk_result != VK_SUCCESS)
            return stepFromVk(vk_result, "bridge_import_failed", "vkBindImageMemory");
        return {};
    }

    const bool create_device_;
    const bool has_gpu_;
    std::string fault_;
    bool late_fired_ = false;
    bool event_fail_fired_ = false;
    int create_calls_ = 0;
    std::unique_ptr<SenderDx> sender_;
    std::string actual_;
    bool published_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkImportSemaphoreWin32HandleKHR import_semaphore_ = nullptr;
    PFN_vkGetMemoryWin32HandlePropertiesKHR handle_properties_ = nullptr;
    ComPtr<ID3D11Device> warp_device_;
    ComPtr<ID3D11Device5> device5_;
    ComPtr<ID3D11DeviceContext4> context4_;
    ComPtr<ID3D11Fence> shared_fence_;
    ComPtr<ID3D11Fence> done_fence_;
    HANDLE event_ = nullptr;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    std::uint64_t done_value_ = 0;
    std::uint64_t pending_done_ = 0;
    std::array<Bridge, output_spout_bridge_count> bridges_{};
};
} // namespace

OutputSpoutBackendFactory outputSpoutRealBackendFactory(OutputSpoutGpuRequest request) {
    disableSpoutLogsOnce();
    return [request = std::move(request)] {
        return std::make_unique<SpoutDxBackend>(true, request);
    };
}

#if PELICAN_OUTPUT_TEST_SEAMS
std::unique_ptr<OutputSpoutBackend> makeOutputSpoutBackendWithoutDeviceForTesting() {
    disableSpoutLogsOnce();
    return std::make_unique<SpoutDxBackend>(false);
}
#endif

} // namespace Pelican
