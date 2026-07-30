#pragma once

#include "frameplanner.hpp"
#include "renderingpass.hpp"
#include "../../project/vulkancompletephysicalplan.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FrameGraphResourceContainer;
class FrameResources;
struct FrameRenderContext;
class PipelineFactory;
class RenderTargetContainer;
class RenderTargetLayoutTracker;
class ShaderLibrary;
class VulkanManageCore;
class VulkanUtils;

inline constexpr std::string_view
    builtinVulkanNoopMarkerNativeScope =
        "builtin.vulkan.noop_marker@1";
inline constexpr std::uint32_t
    vulkanNativeScopeExecutorProviderApiVersion = 1;

enum class VulkanNativeScopeExecutorCapability : std::uint64_t {
    automatic_synchronization = 1ull << 0u,
    manual_synchronization = 1ull << 1u,
    unchecked_synchronization = 1ull << 2u,
    capture_compatible = 1ull << 3u,
    device_loss_recoverable = 1ull << 4u,
    hot_reloadable = 1ull << 5u,
};

using VulkanNativeScopeExecutorCapabilities = std::uint64_t;

constexpr VulkanNativeScopeExecutorCapabilities
vulkanNativeScopeExecutorCapability(
    VulkanNativeScopeExecutorCapability capability) noexcept {
    return static_cast<VulkanNativeScopeExecutorCapabilities>(
        capability);
}

constexpr bool hasVulkanNativeScopeExecutorCapability(
    VulkanNativeScopeExecutorCapabilities capabilities,
    VulkanNativeScopeExecutorCapability capability) noexcept {
    return (capabilities &
            vulkanNativeScopeExecutorCapability(capability)) != 0;
}

struct VulkanNativeScopeDeviceContext {
    vk::Device device;
    vk::PhysicalDevice physical_device;
    std::uint32_t graphics_queue_family = 0;
    // Source extensions may use existing engine allocation/pipeline services.
    // These are intentionally not part of a frozen game-DLL ABI.
    VulkanManageCore *vulkan_core = nullptr;
    PipelineFactory *pipeline_factory = nullptr;
    ShaderLibrary *shader_library = nullptr;
};

enum class VulkanNativeScopePreparedResourceKind : std::uint8_t {
    render_target_image,
    frame_target_image,
    frame_graph_buffer,
    provider_owned,
};

std::string_view vulkanNativeScopePreparedResourceKindName(
    VulkanNativeScopePreparedResourceKind kind);

struct VulkanNativeScopePreparedResource {
    VulkanNativeScopeResourceBoundary boundary;
    VulkanPhysicalResourcePlan physical;
    VulkanNativeScopePreparedResourceKind kind =
        VulkanNativeScopePreparedResourceKind::render_target_image;
    GlobalRenderTargetId render_target = noRenderTargetId();
    FrameGraphBufferId buffer = noFrameGraphBufferId();
    std::vector<VulkanPhysicalAttachmentPlan> attachments;
    vk::ImageLayout required_resolved_layout =
        vk::ImageLayout::eUndefined;
    vk::ImageLayout required_attachment_layout =
        vk::ImageLayout::eUndefined;
};

struct VulkanNativeScopePrepareContext {
    VulkanNativeScopeDeviceContext device;
    std::string_view graph;
    std::uint64_t package_fingerprint = 0;
    const VulkanNativeScopeDeclaration &declaration;
    const VulkanPhysicalScopePlan &scope;
    std::span<const VulkanNativeScopePreparedResource>
        resources;
};

struct VulkanNativeScopeRuntimeAttachment {
    std::string node;
    VulkanPhysicalAttachmentAspect aspect =
        VulkanPhysicalAttachmentAspect::color;
    std::optional<ImageSubresourceRange> subresource;
    vk::ImageView image_view;
    vk::ImageView resolve_image_view;
    vk::Extent2D extent;
};

struct VulkanNativeScopeRuntimeImage {
    vk::Image resolved_image;
    vk::ImageView resolved_view;
    vk::ImageLayout resolved_layout =
        vk::ImageLayout::eUndefined;
    vk::ImageLayout required_resolved_layout =
        vk::ImageLayout::eUndefined;
    vk::Image attachment_image;
    vk::ImageView attachment_view;
    vk::ImageLayout attachment_layout =
        vk::ImageLayout::eUndefined;
    vk::ImageLayout required_attachment_layout =
        vk::ImageLayout::eUndefined;
    vk::Format format = vk::Format::eUndefined;
    vk::Extent3D extent{};
    vk::SampleCountFlagBits samples =
        vk::SampleCountFlagBits::e1;
    std::uint32_t array_layers = 1;
    std::vector<VulkanNativeScopeRuntimeAttachment>
        attachments;
};

struct VulkanNativeScopeRuntimeBuffer {
    vk::Buffer buffer;
    vk::DeviceSize size = 0;
};

struct VulkanNativeScopeRuntimeResource {
    const VulkanNativeScopePreparedResource *contract =
        nullptr;
    VulkanNativeScopeRuntimeImage image;
    VulkanNativeScopeRuntimeBuffer buffer;
};

struct VulkanNativeScopeRecordContext {
    vk::CommandBuffer command_buffer;
    const FrameRenderContext *frame = nullptr;
    const FrameResources *frame_resources = nullptr;
    const VulkanNativeScopeDeclaration *declaration =
        nullptr;
    const VulkanPhysicalScopePlan *scope = nullptr;
    RenderPassViewInvocation view;
    std::span<const VulkanNativeScopeRuntimeResource>
        resources;
};

class VulkanNativeScopeExecutor {
  public:
    virtual ~VulkanNativeScopeExecutor() = default;
    virtual void record(
        const VulkanNativeScopeRecordContext &context) const = 0;
};

using VulkanNativeScopePrepareCallback =
    std::function<std::shared_ptr<const VulkanNativeScopeExecutor>(
        const VulkanNativeScopePrepareContext &)>;

struct VulkanNativeScopeExecutorProvider {
    std::uint32_t api_version =
        vulkanNativeScopeExecutorProviderApiVersion;
    std::string provider;
    std::string implementation;
    VulkanNativeScopeExecutorCapabilities capabilities = 0;
    std::vector<std::string> queue_capabilities{
        "pelican.vulkan.graphics@1"};
    // A non-engine provider must supply a strong lease that keeps the code
    // containing prepare and executor destructors loaded.
    std::shared_ptr<const void> generation_lease;
    VulkanNativeScopePrepareCallback prepare;
};

struct VulkanNativeScopeExecutorProviderHandle {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    explicit operator bool() const noexcept {
        return slot != 0 && generation != 0;
    }
    bool operator==(
        const VulkanNativeScopeExecutorProviderHandle &) const =
        default;
};

struct VulkanNativeScopeExecutorSelection {
    std::string provider;
    std::string implementation;
    internal::RegistrationOwner owner =
        internal::engineRegistrationOwner;
    std::uint32_t registration_generation = 0;
    std::uint32_t provider_api_version = 0;
    VulkanNativeScopeExecutorCapabilities capabilities = 0;

    bool operator==(
        const VulkanNativeScopeExecutorSelection &) const =
        default;
};

class PreparedVulkanNativeScopeExecutor {
    VulkanNativeScopeDeclaration declaration_;
    VulkanPhysicalScopePlan scope_;
    std::vector<VulkanNativeScopePreparedResource>
        resources_;
    VulkanNativeScopeExecutorSelection selection_;
    // Member order is deliberate: executor_ is destroyed before the provider
    // code/generation lease that owns its virtual destructor.
    std::shared_ptr<const void> provider_lifetime_;
    std::shared_ptr<const VulkanNativeScopeExecutor>
        executor_;

  public:
    PreparedVulkanNativeScopeExecutor() = default;
    PreparedVulkanNativeScopeExecutor(
        VulkanNativeScopeDeclaration declaration,
        VulkanPhysicalScopePlan scope,
        std::vector<VulkanNativeScopePreparedResource> resources,
        VulkanNativeScopeExecutorSelection selection,
        std::shared_ptr<const void> provider_lifetime,
        std::shared_ptr<const VulkanNativeScopeExecutor> executor);

    std::string_view scopeId() const noexcept {
        return scope_.id;
    }
    const VulkanNativeScopeDeclaration &declaration() const noexcept {
        return declaration_;
    }
    const VulkanPhysicalScopePlan &scope() const noexcept {
        return scope_;
    }
    std::span<const VulkanNativeScopePreparedResource>
    resources() const noexcept {
        return resources_;
    }
    const VulkanNativeScopeExecutorSelection &selection() const noexcept {
        return selection_;
    }
    void record(VulkanNativeScopeRecordContext context) const;
};

class PreparedVulkanNativeScopeGraph {
    std::string graph_;
    std::uint64_t package_fingerprint_ = 0;
    std::vector<PreparedVulkanNativeScopeExecutor>
        scopes_;

  public:
    PreparedVulkanNativeScopeGraph(
        std::string graph,
        std::uint64_t package_fingerprint,
        std::vector<PreparedVulkanNativeScopeExecutor> scopes);

    std::string_view graph() const noexcept {
        return graph_;
    }
    std::uint64_t packageFingerprint() const noexcept {
        return package_fingerprint_;
    }
    std::span<const PreparedVulkanNativeScopeExecutor>
    scopes() const noexcept {
        return scopes_;
    }
    const PreparedVulkanNativeScopeExecutor *find(
        std::string_view scope) const noexcept;
};

class VulkanNativeScopeExecutorRegistrySnapshot {
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    explicit VulkanNativeScopeExecutorRegistrySnapshot(
        std::shared_ptr<const Impl> impl) noexcept;
    friend class VulkanNativeScopeExecutorRegistry;

  public:
    VulkanNativeScopeExecutorRegistrySnapshot() = default;

    PreparedVulkanNativeScopeExecutor prepare(
        const VulkanNativeScopeDeclaration &declaration,
        const VulkanPhysicalScopePlan &scope,
        std::vector<VulkanNativeScopePreparedResource> resources,
        VulkanNativeScopeDeviceContext device,
        std::string_view graph,
        std::uint64_t package_fingerprint) const;
};

class VulkanNativeScopeExecutorRegistry {
    struct State;
    std::unique_ptr<State> state_;

  public:
    VulkanNativeScopeExecutorRegistry();
    ~VulkanNativeScopeExecutorRegistry();
    VulkanNativeScopeExecutorRegistry(
        const VulkanNativeScopeExecutorRegistry &) = delete;
    VulkanNativeScopeExecutorRegistry &operator=(
        const VulkanNativeScopeExecutorRegistry &) = delete;

    VulkanNativeScopeExecutorProviderHandle registerProvider(
        VulkanNativeScopeExecutorProvider provider,
        internal::RegistrationOwner owner =
            internal::engineRegistrationOwner);
    bool unregisterProvider(
        VulkanNativeScopeExecutorProviderHandle handle,
        internal::RegistrationOwner owner) noexcept;
    VulkanNativeScopeExecutorRegistrySnapshot snapshot() const;
    void activateOwner(
        internal::RegistrationOwner owner) noexcept;
    void releaseOwner(
        internal::RegistrationOwner owner) noexcept;
};

VulkanNativeScopeExecutorRegistry &
vulkanNativeScopeExecutorRegistry();

namespace vulkan_native_scope_internal {
void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept;
void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept;
}

std::shared_ptr<const PreparedVulkanNativeScopeGraph>
prepareVulkanNativeScopeExecutors(
    const VulkanNativeScopeExecutorRegistrySnapshot &providers,
    const VerifiedVulkanCompletePhysicalPlanPackage &verified,
    const VulkanTargetPlan &target_plan,
    const FramePlan &frame_plan,
    const std::unordered_map<std::string, GlobalRenderTargetId>
        &render_target_bindings,
    const std::unordered_map<std::string, FrameGraphBufferId>
        &buffer_bindings,
    VulkanNativeScopeDeviceContext device);

struct VulkanNativeScopeRuntimeInvocation {
    const PreparedVulkanNativeScopeExecutor *prepared =
        nullptr;
    RenderPassViewInvocation view;
    std::vector<VulkanNativeScopeRuntimeResource>
        resources;
};

VulkanNativeScopeRuntimeInvocation
beginVulkanNativeScopeRuntimeInvocation(
    const PreparedVulkanNativeScopeExecutor &prepared,
    const FrameRenderContext &frame,
    vk::Format frame_target_format,
    RenderPassViewInvocation view,
    RenderTargetContainer &render_targets,
    FrameGraphResourceContainer &frame_graph_resources,
    VulkanUtils &vulkan_utils,
    RenderTargetLayoutTracker &layout_tracker);

void recordVulkanNativeScopeRuntimeInvocation(
    const VulkanNativeScopeRuntimeInvocation &invocation,
    const FrameRenderContext &frame,
    const FrameResources &frame_resources);

void endVulkanNativeScopeRuntimeInvocation(
    const VulkanNativeScopeRuntimeInvocation &invocation,
    RenderTargetContainer &render_targets,
    RenderTargetLayoutTracker &layout_tracker);

} // namespace Pelican
