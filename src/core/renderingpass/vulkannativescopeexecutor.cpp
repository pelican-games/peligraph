#include "vulkannativescopeexecutor.hpp"

#include "computetask.hpp"
#include "rendertargetcontainer.hpp"
#include "../renderer/frameresources.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/frametarget.hpp"
#include "../vkcore/render_target_layout_tracker.hpp"
#include "../vkcore/util.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

class NoopMarkerNativeScopeExecutor final
    : public VulkanNativeScopeExecutor {
  public:
    void record(
        const VulkanNativeScopeRecordContext &) const override {}
};

struct ProviderEntry {
    // Destruction order keeps the code lease alive while std::function and
    // provider-owned state are destroyed.
    std::shared_ptr<const void> generation_lease;
    VulkanNativeScopeExecutorProvider provider;
    internal::RegistrationOwner owner =
        internal::engineRegistrationOwner;
    std::uint32_t registration_generation = 0;
    std::uint32_t slot = 0;
};

VulkanNativeScopeExecutorCapability synchronizationCapability(
    VulkanNativeScopeSynchronizationMode mode) {
    switch (mode) {
    case VulkanNativeScopeSynchronizationMode::automatic:
        return VulkanNativeScopeExecutorCapability::
            automatic_synchronization;
    case VulkanNativeScopeSynchronizationMode::manual:
        return VulkanNativeScopeExecutorCapability::
            manual_synchronization;
    case VulkanNativeScopeSynchronizationMode::unchecked:
        return VulkanNativeScopeExecutorCapability::
            unchecked_synchronization;
    }
    throw std::runtime_error(
        "NativeScope declaration has an unknown synchronization mode");
}

void validateProviderDefinition(
    const VulkanNativeScopeExecutorProvider &provider,
    internal::RegistrationOwner owner) {
    if (provider.api_version !=
        vulkanNativeScopeExecutorProviderApiVersion) {
        throw std::runtime_error(
            "NativeScope executor provider has an unsupported API version");
    }
    if (provider.provider.empty() ||
        provider.implementation.empty() ||
        provider.queue_capabilities.empty() ||
        !provider.prepare) {
        throw std::runtime_error(
            "NativeScope executor provider is incomplete");
    }
    if (std::any_of(
            provider.queue_capabilities.begin(),
            provider.queue_capabilities.end(),
            [](const auto &capability) {
                return capability.empty();
            })) {
        throw std::runtime_error(
            "NativeScope executor provider has an empty queue capability");
    }
    if (owner != internal::engineRegistrationOwner &&
        provider.generation_lease == nullptr) {
        throw std::runtime_error(
            "Non-engine NativeScope executor provider requires a "
            "generation lease");
    }
    if (!internal::isRegistrationOwnerCurrent(owner)) {
        throw std::runtime_error(
            "Stale NativeScope executor provider owner was rejected");
    }
}

std::uint32_t nextGeneration(std::uint32_t value) {
    if (value == std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "NativeScope executor provider generation is exhausted");
    }
    ++value;
    if (value == 0) ++value;
    return value;
}

vk::ImageLayout requiredImageLayout(
    const VulkanNativeScopeResourceBoundary &boundary,
    const VulkanPhysicalScopePlan &scope,
    std::span<const VulkanPhysicalAttachmentPlan>
        attachments) {
    if (!attachments.empty()) {
        const auto aspect = attachments.front().aspect;
        if (std::any_of(
                attachments.begin(), attachments.end(),
                [aspect](const auto &attachment) {
                    return attachment.aspect != aspect;
                })) {
            throw std::runtime_error(
                "NativeScope resource is used as both color and depth "
                "inside one physical scope: " +
                boundary.logical_resource);
        }
        return aspect ==
                       VulkanPhysicalAttachmentAspect::depth
                   ? vk::ImageLayout::eDepthAttachmentOptimal
                   : vk::ImageLayout::eColorAttachmentOptimal;
    }

    switch (scope.kind) {
    case VulkanPhysicalScopeKind::compute:
        return boundary.access == LogicalAccessMode::read
                   ? vk::ImageLayout::eShaderReadOnlyOptimal
                   : vk::ImageLayout::eGeneral;
    case VulkanPhysicalScopeKind::transfer:
        if (boundary.access == LogicalAccessMode::read) {
            return vk::ImageLayout::eTransferSrcOptimal;
        }
        if (boundary.access == LogicalAccessMode::write) {
            return vk::ImageLayout::eTransferDstOptimal;
        }
        return vk::ImageLayout::eGeneral;
    case VulkanPhysicalScopeKind::output:
        return boundary.access == LogicalAccessMode::read
                   ? vk::ImageLayout::eShaderReadOnlyOptimal
                   : vk::ImageLayout::eColorAttachmentOptimal;
    case VulkanPhysicalScopeKind::rendering:
    case VulkanPhysicalScopeKind::marker:
        return boundary.access == LogicalAccessMode::read
                   ? vk::ImageLayout::eShaderReadOnlyOptimal
                   : vk::ImageLayout::eGeneral;
    }
    throw std::runtime_error(
        "NativeScope physical scope has an unknown kind");
}

bool scopeContainsNode(
    const VulkanPhysicalScopePlan &scope,
    std::string_view node) {
    return std::find(
               scope.nodes.begin(), scope.nodes.end(),
               node) != scope.nodes.end();
}

std::vector<VulkanPhysicalAttachmentPlan>
resourceAttachments(
    const VulkanCompletePhysicalPlanPackage &package,
    const VulkanPhysicalScopePlan &scope,
    std::string_view resource) {
    std::vector<VulkanPhysicalAttachmentPlan> result;
    for (const auto &attachment : package.attachments) {
        if (attachment.logical_resource == resource &&
            scopeContainsNode(scope, attachment.node)) {
            result.push_back(attachment);
        }
    }
    return result;
}

const VulkanPhysicalResourcePlan &requirePhysicalResource(
    const VulkanCompletePhysicalPlanPackage &package,
    std::string_view name) {
    const auto found = std::find_if(
        package.resources.begin(), package.resources.end(),
        [name](const auto &resource) {
            return resource.logical_resource == name;
        });
    if (found == package.resources.end()) {
        throw std::runtime_error(
            "NativeScope boundary references an unknown physical "
            "resource: " +
            std::string{name});
    }
    return *found;
}

const VulkanPhysicalScopePlan &requirePhysicalScope(
    const VulkanCompletePhysicalPlanPackage &package,
    std::string_view id) {
    const auto found = std::find_if(
        package.scopes.begin(), package.scopes.end(),
        [id](const auto &scope) {
            return scope.id == id;
        });
    if (found == package.scopes.end()) {
        throw std::runtime_error(
            "NativeScope executor references an unknown physical scope: " +
            std::string{id});
    }
    return *found;
}

VulkanNativeScopePreparedResource prepareResource(
    const VulkanCompletePhysicalPlanPackage &package,
    const VulkanPhysicalScopePlan &scope,
    const VulkanNativeScopeResourceBoundary &boundary,
    const std::unordered_map<std::string, GlobalRenderTargetId>
        &render_target_bindings,
    const std::unordered_map<std::string, FrameGraphBufferId>
        &buffer_bindings) {
    VulkanNativeScopePreparedResource result;
    result.boundary = boundary;
    result.physical =
        requirePhysicalResource(package, boundary.logical_resource);
    result.attachments =
        resourceAttachments(
            package, scope, boundary.logical_resource);

    if (boundary.ownership ==
        VulkanNativeScopeResourceOwnership::native_scope) {
        result.kind =
            VulkanNativeScopePreparedResourceKind::provider_owned;
        return result;
    }

    if (result.physical.representation ==
        VulkanResourceRepresentation::materialized_buffer) {
        const auto found =
            buffer_bindings.find(boundary.logical_resource);
        if (found == buffer_bindings.end() ||
            !isValidFrameGraphBufferId(found->second)) {
            throw std::runtime_error(
                "NativeScope engine-owned buffer has no runtime "
                "binding: " +
                boundary.logical_resource);
        }
        result.kind =
            VulkanNativeScopePreparedResourceKind::frame_graph_buffer;
        result.buffer = found->second;
        return result;
    }

    const auto image =
        render_target_bindings.find(boundary.logical_resource);
    if (image != render_target_bindings.end() &&
        isConcreteRenderTarget(image->second)) {
        result.kind =
            VulkanNativeScopePreparedResourceKind::render_target_image;
        result.render_target = image->second;
    } else if (
        result.physical.representation ==
            VulkanResourceRepresentation::external &&
        boundary.logical_resource == "swapchain") {
        result.kind =
            VulkanNativeScopePreparedResourceKind::frame_target_image;
    } else {
        throw std::runtime_error(
            "NativeScope engine-owned image has no runtime binding: " +
            boundary.logical_resource);
    }

    const auto layout =
        requiredImageLayout(
            boundary, scope, result.attachments);
    result.required_resolved_layout = layout;
    result.required_attachment_layout =
        !result.attachments.empty()
            ? layout
            : vk::ImageLayout::eUndefined;
    return result;
}

void validateFramePlanCoverage(
    const FramePlan &frame_plan,
    const VulkanCompletePhysicalPlanPackage &package) {
    if (frame_plan.name != package.graph) {
        throw std::runtime_error(
            "NativeScope complete package and frame plan graph differ");
    }
    std::unordered_set<std::string> planned;
    for (const auto &node : frame_plan.nodes) {
        planned.insert(node.name);
    }
    for (const auto &scope : package.scopes) {
        for (const auto &node : scope.nodes) {
            if (!planned.contains(node)) {
                throw std::runtime_error(
                    "NativeScope physical scope node is absent from the "
                    "runtime frame plan: " +
                    node);
            }
        }
    }
}

vk::ImageView resolvedImageView(
    const VulkanNativeScopePreparedResource &resource,
    const RenderTargetContainer &targets,
    RenderPassViewInvocation view) {
    switch (resource.physical.view_layout) {
    case VulkanResourceViewLayout::shared_2d:
        return targets.getImageView(resource.render_target);
    case VulkanResourceViewLayout::sequential_2d:
        if (view.logical_view_count == 0 ||
            view.view_index >= view.logical_view_count) {
            throw std::runtime_error(
                "NativeScope sequential image view is out of range");
        }
        return targets.getImageLayerView(
            resource.render_target, view.view_index);
    case VulkanResourceViewLayout::layered_2d_array:
    case VulkanResourceViewLayout::family_2d_array:
        return targets.getLayeredImageView(
            resource.render_target);
    }
    throw std::runtime_error(
        "NativeScope image has an unknown physical view layout");
}

vk::ImageView attachmentImageView(
    const VulkanNativeScopePreparedResource &resource,
    const VulkanPhysicalScopePlan &scope,
    const RenderTargetContainer &targets,
    RenderPassViewInvocation view) {
    if (scope.view_execution ==
        VulkanScopeViewExecution::multiview) {
        return targets.getLayeredAttachmentImageView(
            resource.render_target);
    }
    const auto metadata =
        targets.getMetadata(resource.render_target);
    const auto layer =
        scope.view_execution ==
                    VulkanScopeViewExecution::sequential &&
                metadata.array_layers >=
                    view.logical_view_count
            ? view.view_index
            : 0u;
    return targets.getAttachmentImageLayerView(
        resource.render_target, layer);
}

ImageSubresourceRange invocationSubresource(
    ImageSubresourceRange authored,
    const VulkanPhysicalScopePlan &scope,
    RenderPassViewInvocation view) {
    if (scope.view_execution ==
        VulkanScopeViewExecution::sequential) {
        if (view.logical_view_count == 0 ||
            view.view_index >= view.logical_view_count ||
            authored.layer_count <
                view.logical_view_count) {
            throw std::runtime_error(
                "NativeScope attachment subresource does not fit its "
                "sequential view execution");
        }
        authored.base_array_layer +=
            view.view_index;
        authored.layer_count = 1;
    }
    return authored;
}

vk::Extent2D mipExtent(
    vk::Extent2D extent,
    std::uint32_t mip_level) {
    return {
        std::max(1u, extent.width >> mip_level),
        std::max(1u, extent.height >> mip_level),
    };
}

VulkanNativeScopeRuntimeAttachment runtimeAttachment(
    const VulkanNativeScopePreparedResource &resource,
    const VulkanPhysicalAttachmentPlan &attachment,
    const VulkanPhysicalScopePlan &scope,
    const RenderTargetContainer &targets,
    RenderPassViewInvocation view) {
    const auto metadata =
        targets.getMetadata(resource.render_target);
    VulkanNativeScopeRuntimeAttachment result{
        .node = attachment.node,
        .aspect = attachment.aspect,
        .subresource = attachment.subresource,
        .extent = metadata.extent,
    };
    if (!attachment.subresource) {
        result.image_view =
            attachmentImageView(
                resource, scope, targets, view);
        result.resolve_image_view =
            targets.hasSeparateAttachment(
                    resource.render_target)
                ? resolvedImageView(resource, targets, view)
                : vk::ImageView{};
        return result;
    }

    const auto subresource =
        invocationSubresource(
            *attachment.subresource, scope, view);
    const bool array_view =
        scope.view_execution ==
        VulkanScopeViewExecution::multiview;
    result.image_view =
        targets.getAttachmentImageSubresourceView(
            resource.render_target, subresource,
            array_view);
    result.resolve_image_view =
        targets.hasSeparateAttachment(
                resource.render_target)
            ? targets.getImageSubresourceView(
                  resource.render_target,
                  subresource, array_view)
            : vk::ImageView{};
    result.extent =
        mipExtent(
            metadata.extent,
            subresource.base_mip_level);
    result.subresource = subresource;
    return result;
}

} // namespace

std::string_view vulkanNativeScopePreparedResourceKindName(
    VulkanNativeScopePreparedResourceKind kind) {
    switch (kind) {
    case VulkanNativeScopePreparedResourceKind::
        render_target_image:
        return "render_target_image";
    case VulkanNativeScopePreparedResourceKind::
        frame_target_image:
        return "frame_target_image";
    case VulkanNativeScopePreparedResourceKind::
        frame_graph_buffer:
        return "frame_graph_buffer";
    case VulkanNativeScopePreparedResourceKind::
        provider_owned:
        return "provider_owned";
    }
    throw std::runtime_error(
        "Unknown prepared NativeScope resource kind");
}

struct VulkanNativeScopeExecutorRegistrySnapshot::Impl {
    std::vector<std::shared_ptr<const ProviderEntry>>
        providers;
    internal::RegistrationOwner active_owner =
        internal::engineRegistrationOwner;
};

struct VulkanNativeScopeExecutorRegistry::State {
    struct Slot {
        std::uint32_t generation = 0;
        std::shared_ptr<const ProviderEntry> entry;
    };

    mutable std::shared_mutex mutex;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> free_slots;
    internal::RegistrationOwner active_owner =
        internal::engineRegistrationOwner;
};

PreparedVulkanNativeScopeExecutor::
    PreparedVulkanNativeScopeExecutor(
        VulkanNativeScopeDeclaration declaration,
        VulkanPhysicalScopePlan scope,
        std::vector<VulkanNativeScopePreparedResource> resources,
        VulkanNativeScopeExecutorSelection selection,
        std::shared_ptr<const void> provider_lifetime,
        std::shared_ptr<const VulkanNativeScopeExecutor> executor)
    : declaration_{std::move(declaration)},
      scope_{std::move(scope)},
      resources_{std::move(resources)},
      selection_{std::move(selection)},
      provider_lifetime_{std::move(provider_lifetime)},
      executor_{std::move(executor)} {
    if (executor_ == nullptr ||
        provider_lifetime_ == nullptr) {
        throw std::invalid_argument(
            "Prepared NativeScope executor requires provider lifetime "
            "and executable state");
    }
}

void PreparedVulkanNativeScopeExecutor::record(
    VulkanNativeScopeRecordContext context) const {
    if (executor_ == nullptr) {
        throw std::logic_error(
            "Prepared NativeScope executor is empty");
    }
    context.declaration = &declaration_;
    context.scope = &scope_;
    executor_->record(context);
}

PreparedVulkanNativeScopeGraph::
    PreparedVulkanNativeScopeGraph(
        std::string graph,
        std::uint64_t package_fingerprint,
        std::vector<PreparedVulkanNativeScopeExecutor> scopes)
    : graph_{std::move(graph)},
      package_fingerprint_{package_fingerprint},
      scopes_{std::move(scopes)} {
    if (graph_.empty() || package_fingerprint_ == 0) {
        throw std::invalid_argument(
            "Prepared NativeScope graph requires graph identity and "
            "package fingerprint");
    }
}

const PreparedVulkanNativeScopeExecutor *
PreparedVulkanNativeScopeGraph::find(
    std::string_view scope) const noexcept {
    const auto found = std::find_if(
        scopes_.begin(), scopes_.end(),
        [scope](const auto &candidate) {
            return candidate.scopeId() == scope;
        });
    return found != scopes_.end() ? &*found : nullptr;
}

VulkanNativeScopeExecutorRegistrySnapshot::
    VulkanNativeScopeExecutorRegistrySnapshot(
        std::shared_ptr<const Impl> impl) noexcept
    : impl_{std::move(impl)} {}

PreparedVulkanNativeScopeExecutor
VulkanNativeScopeExecutorRegistrySnapshot::prepare(
    const VulkanNativeScopeDeclaration &declaration,
    const VulkanPhysicalScopePlan &scope,
    std::vector<VulkanNativeScopePreparedResource> resources,
    VulkanNativeScopeDeviceContext device,
    std::string_view graph,
    std::uint64_t package_fingerprint) const {
    if (impl_ == nullptr) {
        throw std::runtime_error(
            "NativeScope executor provider snapshot is empty");
    }
    std::shared_ptr<const ProviderEntry> selected;
    const auto find_for_owner =
        [&](internal::RegistrationOwner owner) {
            return std::find_if(
                impl_->providers.begin(),
                impl_->providers.end(),
                [&](const auto &entry) {
                    return entry->owner == owner &&
                           entry->provider.implementation ==
                               declaration.implementation;
                });
        };
    if (impl_->active_owner !=
        internal::engineRegistrationOwner) {
        const auto found =
            find_for_owner(impl_->active_owner);
        if (found != impl_->providers.end()) {
            selected = *found;
        }
    }
    if (selected == nullptr) {
        const auto found =
            find_for_owner(
                internal::engineRegistrationOwner);
        if (found != impl_->providers.end()) {
            selected = *found;
        }
    }
    if (selected == nullptr) {
        throw std::runtime_error(
            "NativeScope executor provider was not found for "
            "implementation '" +
            declaration.implementation + "'");
    }

    const auto required_sync =
        synchronizationCapability(
            declaration.synchronization);
    if (!hasVulkanNativeScopeExecutorCapability(
            selected->provider.capabilities,
            required_sync)) {
        throw std::runtime_error(
            "NativeScope executor provider does not support requested "
            "synchronization mode: " +
            declaration.implementation);
    }
    if (std::find(
            selected->provider.queue_capabilities.begin(),
            selected->provider.queue_capabilities.end(),
            declaration.queue_capability) ==
        selected->provider.queue_capabilities.end()) {
        throw std::runtime_error(
            "NativeScope executor provider does not support queue "
            "capability '" +
            declaration.queue_capability + "'");
    }
    const auto require_capability =
        [&](bool requested,
            VulkanNativeScopeExecutorCapability capability,
            std::string_view name) {
            if (requested &&
                !hasVulkanNativeScopeExecutorCapability(
                    selected->provider.capabilities,
                    capability)) {
                throw std::runtime_error(
                    "NativeScope executor provider does not satisfy "
                    "declared " +
                    std::string{name} + " capability");
            }
        };
    require_capability(
        declaration.capture_compatible,
        VulkanNativeScopeExecutorCapability::capture_compatible,
        "capture");
    require_capability(
        declaration.device_loss_recoverable,
        VulkanNativeScopeExecutorCapability::
            device_loss_recoverable,
        "device-loss recovery");
    require_capability(
        declaration.hot_reloadable,
        VulkanNativeScopeExecutorCapability::hot_reloadable,
        "hot-reload");

    const VulkanNativeScopePrepareContext context{
        .device = device,
        .graph = graph,
        .package_fingerprint = package_fingerprint,
        .declaration = declaration,
        .scope = scope,
        .resources = resources,
    };
    auto executor =
        selected->provider.prepare(context);
    if (executor == nullptr) {
        throw std::runtime_error(
            "NativeScope executor provider returned no executable state: " +
            declaration.implementation);
    }
    return PreparedVulkanNativeScopeExecutor{
        declaration,
        scope,
        std::move(resources),
        VulkanNativeScopeExecutorSelection{
            .provider = selected->provider.provider,
            .implementation =
                selected->provider.implementation,
            .owner = selected->owner,
            .registration_generation =
                selected->registration_generation,
            .provider_api_version =
                selected->provider.api_version,
            .capabilities =
                selected->provider.capabilities,
        },
        selected,
        std::move(executor),
    };
}

VulkanNativeScopeExecutorRegistry::
    VulkanNativeScopeExecutorRegistry()
    : state_{std::make_unique<State>()} {
    const auto sync_capabilities =
        vulkanNativeScopeExecutorCapability(
            VulkanNativeScopeExecutorCapability::
                automatic_synchronization) |
        vulkanNativeScopeExecutorCapability(
            VulkanNativeScopeExecutorCapability::
                manual_synchronization) |
        vulkanNativeScopeExecutorCapability(
            VulkanNativeScopeExecutorCapability::
                unchecked_synchronization);
    (void)registerProvider(
        VulkanNativeScopeExecutorProvider{
            .api_version =
                vulkanNativeScopeExecutorProviderApiVersion,
            .provider = "builtin.vulkan",
            .implementation =
                std::string{
                    builtinVulkanNoopMarkerNativeScope},
            .capabilities =
                sync_capabilities |
                vulkanNativeScopeExecutorCapability(
                    VulkanNativeScopeExecutorCapability::
                        capture_compatible) |
                vulkanNativeScopeExecutorCapability(
                    VulkanNativeScopeExecutorCapability::
                        device_loss_recoverable),
            .queue_capabilities = {
                "pelican.vulkan.graphics@1"},
            .prepare =
                [](const VulkanNativeScopePrepareContext
                       &context)
                -> std::shared_ptr<
                    const VulkanNativeScopeExecutor> {
                    if (context.scope.kind !=
                            VulkanPhysicalScopeKind::marker ||
                        !context.resources.empty()) {
                        throw std::runtime_error(
                            "builtin no-op NativeScope accepts only an "
                            "empty marker scope");
                    }
                    return std::make_shared<
                        NoopMarkerNativeScopeExecutor>();
                },
        });
}

VulkanNativeScopeExecutorRegistry::
    ~VulkanNativeScopeExecutorRegistry() = default;

VulkanNativeScopeExecutorProviderHandle
VulkanNativeScopeExecutorRegistry::registerProvider(
    VulkanNativeScopeExecutorProvider provider,
    internal::RegistrationOwner owner) {
    validateProviderDefinition(provider, owner);
    std::unique_lock lock{state_->mutex};
    for (const auto &slot : state_->slots) {
        if (slot.entry != nullptr &&
            slot.entry->owner == owner &&
            slot.entry->provider.implementation ==
                provider.implementation) {
            throw std::runtime_error(
                "NativeScope executor provider is already registered for "
                "this owner: " +
                provider.implementation);
        }
    }

    const bool reusing_slot =
        !state_->free_slots.empty();
    std::uint32_t index = 0;
    if (reusing_slot) {
        index = state_->free_slots.back();
    } else {
        if (state_->slots.size() ==
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(
                "NativeScope executor provider registry is exhausted");
        }
        index =
            static_cast<std::uint32_t>(
                state_->slots.size());
    }
    const auto generation =
        reusing_slot
            ? nextGeneration(
                  state_->slots[index].generation)
            : 1u;
    auto entry =
        std::make_shared<ProviderEntry>(
            ProviderEntry{
                .generation_lease =
                    provider.generation_lease,
                .provider = std::move(provider),
                .owner = owner,
                .registration_generation =
                    generation,
                .slot = index + 1,
            });
    if (reusing_slot) {
        state_->free_slots.pop_back();
        auto &slot = state_->slots[index];
        slot.generation = generation;
        slot.entry = std::move(entry);
    } else {
        state_->slots.push_back(
            State::Slot{
                .generation = generation,
                .entry = std::move(entry),
            });
    }
    return {
        .slot = index + 1,
        .generation = generation,
    };
}

bool VulkanNativeScopeExecutorRegistry::unregisterProvider(
    VulkanNativeScopeExecutorProviderHandle handle,
    internal::RegistrationOwner owner) noexcept {
    if (!handle) return false;
    std::unique_lock lock{state_->mutex};
    const auto index = handle.slot - 1;
    if (index >= state_->slots.size()) return false;
    auto &slot = state_->slots[index];
    if (slot.generation != handle.generation ||
        slot.entry == nullptr ||
        slot.entry->owner != owner) {
        return false;
    }
    slot.entry.reset();
    state_->free_slots.push_back(index);
    return true;
}

VulkanNativeScopeExecutorRegistrySnapshot
VulkanNativeScopeExecutorRegistry::snapshot() const {
    auto result =
        std::make_shared<
            VulkanNativeScopeExecutorRegistrySnapshot::Impl>();
    std::shared_lock lock{state_->mutex};
    result->active_owner = state_->active_owner;
    for (const auto &slot : state_->slots) {
        if (slot.entry != nullptr) {
            result->providers.push_back(slot.entry);
        }
    }
    return VulkanNativeScopeExecutorRegistrySnapshot{
        std::move(result)};
}

void VulkanNativeScopeExecutorRegistry::activateOwner(
    internal::RegistrationOwner owner) noexcept {
    std::unique_lock lock{state_->mutex};
    state_->active_owner =
        owner != internal::engineRegistrationOwner &&
                internal::isRegistrationOwnerCurrent(owner)
            ? owner
            : internal::engineRegistrationOwner;
}

void VulkanNativeScopeExecutorRegistry::releaseOwner(
    internal::RegistrationOwner owner) noexcept {
    if (owner == internal::engineRegistrationOwner) return;
    std::unique_lock lock{state_->mutex};
    if (state_->active_owner == owner) {
        state_->active_owner =
            internal::engineRegistrationOwner;
    }
    for (std::uint32_t index = 0;
         index < state_->slots.size(); ++index) {
        auto &slot = state_->slots[index];
        if (slot.entry != nullptr &&
            slot.entry->owner == owner) {
            slot.entry.reset();
            state_->free_slots.push_back(index);
        }
    }
}

VulkanNativeScopeExecutorRegistry &
vulkanNativeScopeExecutorRegistry() {
    static auto *registry =
        new VulkanNativeScopeExecutorRegistry;
    return *registry;
}

namespace vulkan_native_scope_internal {

void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        vulkanNativeScopeExecutorRegistry()
            .activateOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        vulkanNativeScopeExecutorRegistry()
            .releaseOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

} // namespace vulkan_native_scope_internal

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
    VulkanNativeScopeDeviceContext device) {
    validateVulkanTargetPlanMatchesCompletePhysicalPackage(
        target_plan, verified);
    const auto &package = verified.package;
    validateFramePlanCoverage(frame_plan, package);
    if (package.native_scopes.empty()) {
        return nullptr;
    }

    std::vector<PreparedVulkanNativeScopeExecutor>
        prepared;
    prepared.reserve(package.native_scopes.size());
    for (const auto &declaration :
         package.native_scopes) {
        const auto &scope =
            requirePhysicalScope(
                package, declaration.scope);
        std::vector<VulkanNativeScopePreparedResource>
            resources;
        resources.reserve(
            declaration.resources.size());
        for (const auto &boundary :
             declaration.resources) {
            resources.push_back(
                prepareResource(
                    package, scope, boundary,
                    render_target_bindings,
                    buffer_bindings));
        }
        prepared.push_back(
            providers.prepare(
                declaration, scope,
                std::move(resources), device,
                package.graph,
                verified.package_fingerprint));
    }
    return std::make_shared<
        const PreparedVulkanNativeScopeGraph>(
        package.graph,
        verified.package_fingerprint,
        std::move(prepared));
}

VulkanNativeScopeRuntimeInvocation
beginVulkanNativeScopeRuntimeInvocation(
    const PreparedVulkanNativeScopeExecutor &prepared,
    const FrameRenderContext &frame,
    vk::Format frame_target_format,
    RenderPassViewInvocation view,
    RenderTargetContainer &render_targets,
    FrameGraphResourceContainer &frame_graph_resources,
    VulkanUtils &vulkan_utils,
    RenderTargetLayoutTracker &layout_tracker) {
    if (view.logical_view_count == 0 ||
        view.view_index >= view.logical_view_count) {
        throw std::runtime_error(
            "NativeScope runtime view invocation is out of range");
    }
    VulkanNativeScopeRuntimeInvocation result{
        .prepared = &prepared,
        .view = view,
    };
    result.resources.reserve(
        prepared.resources().size());

    for (const auto &resource :
         prepared.resources()) {
        if (prepared.declaration().synchronization ==
                VulkanNativeScopeSynchronizationMode::automatic &&
            resource.kind ==
                VulkanNativeScopePreparedResourceKind::
                    render_target_image) {
            if (resource.required_resolved_layout !=
                vk::ImageLayout::eUndefined) {
                layout_tracker.transition(
                    frame.cmd_buf, render_targets,
                    vulkan_utils,
                    resource.render_target,
                    resource.required_resolved_layout);
            }
            if (resource.required_attachment_layout !=
                    vk::ImageLayout::eUndefined &&
                render_targets.hasSeparateAttachment(
                    resource.render_target)) {
                layout_tracker.transition(
                    frame.cmd_buf, render_targets,
                    vulkan_utils,
                    resource.render_target,
                    resource.required_attachment_layout,
                    false,
                    RenderTargetImageKind::attachment);
            }
        }

        VulkanNativeScopeRuntimeResource runtime{
            .contract = &resource,
        };
        switch (resource.kind) {
        case VulkanNativeScopePreparedResourceKind::
            render_target_image: {
            const auto metadata =
                render_targets.getMetadata(
                    resource.render_target);
            const auto &resolved =
                render_targets.getImage(
                    resource.render_target);
            const auto &attachment =
                render_targets.getAttachmentImage(
                    resource.render_target);
            runtime.image.resolved_image =
                resolved.image.get();
            runtime.image.resolved_view =
                resolvedImageView(
                    resource, render_targets, view);
            runtime.image.resolved_layout =
                layout_tracker.currentLayout(
                    resource.render_target, false,
                    &render_targets);
            runtime.image.required_resolved_layout =
                resource.required_resolved_layout;
            runtime.image.attachment_image =
                attachment.image.get();
            runtime.image.attachment_view =
                attachmentImageView(
                    resource, prepared.scope(),
                    render_targets, view);
            runtime.image.attachment_layout =
                layout_tracker.currentLayout(
                    resource.render_target, false,
                    &render_targets,
                    RenderTargetImageKind::attachment);
            runtime.image.required_attachment_layout =
                resource.required_attachment_layout;
            runtime.image.format = metadata.format;
            runtime.image.extent =
                vk::Extent3D{
                    metadata.extent.width,
                    metadata.extent.height, 1};
            runtime.image.samples =
                render_targets.sampleCount(
                    resource.render_target);
            runtime.image.array_layers =
                metadata.array_layers;
            for (const auto &attachment_plan :
                 resource.attachments) {
                runtime.image.attachments.push_back(
                    runtimeAttachment(
                        resource, attachment_plan,
                        prepared.scope(),
                        render_targets, view));
            }
            break;
        }
        case VulkanNativeScopePreparedResourceKind::
            frame_target_image:
            runtime.image.resolved_image =
                frame.color_image;
            runtime.image.attachment_image =
                frame.color_image;
            runtime.image.resolved_view =
                prepared.scope().view_execution ==
                            VulkanScopeViewExecution::sequential &&
                        !frame.color_layer_attachments.empty()
                    ? frame.color_layer_attachments.at(
                          view.view_index)
                    : frame.color_attachment;
            runtime.image.attachment_view =
                runtime.image.resolved_view;
            runtime.image.resolved_layout =
                vk::ImageLayout::eColorAttachmentOptimal;
            runtime.image.attachment_layout =
                vk::ImageLayout::eColorAttachmentOptimal;
            runtime.image.required_resolved_layout =
                resource.required_resolved_layout;
            runtime.image.required_attachment_layout =
                resource.required_attachment_layout;
            runtime.image.format =
                frame_target_format;
            runtime.image.extent =
                vk::Extent3D{
                    frame.extent.width,
                    frame.extent.height, 1};
            runtime.image.array_layers =
                frame.color_array_layers;
            break;
        case VulkanNativeScopePreparedResourceKind::
            frame_graph_buffer: {
            const auto &buffer =
                frame_graph_resources.buffer(
                    resource.buffer);
            runtime.buffer.buffer =
                buffer.buffer.get();
            runtime.buffer.size =
                frame_graph_resources.bufferSize(
                    resource.buffer);
            break;
        }
        case VulkanNativeScopePreparedResourceKind::
            provider_owned:
            break;
        }
        result.resources.push_back(
            std::move(runtime));
    }
    return result;
}

void recordVulkanNativeScopeRuntimeInvocation(
    const VulkanNativeScopeRuntimeInvocation &invocation,
    const FrameRenderContext &frame,
    const FrameResources &frame_resources) {
    if (invocation.prepared == nullptr) {
        throw std::logic_error(
            "NativeScope runtime invocation has no prepared executor");
    }
    invocation.prepared->record(
        VulkanNativeScopeRecordContext{
            .command_buffer = frame.cmd_buf,
            .frame = &frame,
            .frame_resources =
                &frame_resources,
            .view = invocation.view,
            .resources = invocation.resources,
        });
}

void endVulkanNativeScopeRuntimeInvocation(
    const VulkanNativeScopeRuntimeInvocation &invocation,
    RenderTargetContainer &render_targets,
    RenderTargetLayoutTracker &layout_tracker) {
    if (invocation.prepared == nullptr ||
        invocation.prepared->declaration()
                .synchronization ==
            VulkanNativeScopeSynchronizationMode::automatic) {
        return;
    }
    // Manual/unchecked implementations own their barriers but must return
    // every engine-owned boundary image in the declared outer layout.
    for (const auto &runtime :
         invocation.resources) {
        if (runtime.contract == nullptr ||
            runtime.contract->kind !=
                VulkanNativeScopePreparedResourceKind::
                    render_target_image) {
            continue;
        }
        if (runtime.contract
                ->required_resolved_layout !=
            vk::ImageLayout::eUndefined) {
            layout_tracker.assumeLayout(
                render_targets,
                runtime.contract->render_target,
                runtime.contract
                    ->required_resolved_layout);
        }
        if (runtime.contract
                    ->required_attachment_layout !=
                vk::ImageLayout::eUndefined &&
            render_targets.hasSeparateAttachment(
                runtime.contract->render_target)) {
            layout_tracker.assumeLayout(
                render_targets,
                runtime.contract->render_target,
                runtime.contract
                    ->required_attachment_layout,
                false,
                RenderTargetImageKind::attachment);
        }
    }
}

} // namespace Pelican
