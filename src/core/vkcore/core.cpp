#include "core.hpp"
#include "../config.hpp"
#include "../log.hpp"
#include "../os/window.hpp"
#include <optional>
#include <set>
#include <vector>

namespace Pelican {

static vk::UniqueInstance vulkanCreateInstance(Window &window) {
    LOG_INFO(logger, "initializing vulkan instance...");

    vk::ApplicationInfo app_info;
    app_info.pApplicationName = "Pelican App";
    app_info.applicationVersion = 0;
    app_info.pEngineName = engine_name;
    app_info.engineVersion = engine_version;
    app_info.apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 283);

    std::vector<const char *> layers, exts;

#ifdef _DEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    exts = window.getRequiredVulkanInstanceExts();
#ifdef __APPLE__
    create_info.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    vk::InstanceCreateInfo create_info;
    create_info.pApplicationInfo = &app_info;
    create_info.setPEnabledExtensionNames(exts);
    create_info.setPEnabledLayerNames(layers);

    return vk::createInstanceUnique(create_info);
}

static std::optional<QueueSet> pickQueues(const vk::PhysicalDevice &phys_device,
                                          std::vector<vk::QueueFamilyProperties> queue_families,
                                          vk::SurfaceKHR surface) {
    std::optional<uint32_t> graphics_queue;
    std::optional<uint32_t> presentation_queue;
    std::optional<uint32_t> compute_queue;

    for (int i = 0; i < queue_families.size(); i++) {
        if ((queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
            phys_device.getSurfaceSupportKHR(i, surface) &&
            (queue_families[i].queueFlags & vk::QueueFlagBits::eCompute)) {
            graphics_queue = i;
            presentation_queue = i;
            compute_queue = i;
            break;
        }
    }
    if (!graphics_queue || !presentation_queue)
        for (int i = 0; i < queue_families.size(); i++) {
            if ((queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
                phys_device.getSurfaceSupportKHR(i, surface)) {
                graphics_queue = i;
                presentation_queue = i;
                break;
            }
        }
    if (!graphics_queue)
        for (int i = 0; i < queue_families.size(); i++) {
            if (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
                graphics_queue = i;
                break;
            }
        }
    if (!presentation_queue)
        for (int i = 0; i < queue_families.size(); i++) {
            if (phys_device.getSurfaceSupportKHR(i, surface)) {
                presentation_queue = i;
                break;
            }
        }
    if (!compute_queue)
        for (int i = 0; i < queue_families.size(); i++) {
            if (queue_families[i].queueFlags & vk::QueueFlagBits::eCompute) {
                compute_queue = i;
                break;
            }
        }

    if (graphics_queue && presentation_queue && compute_queue) {
        return QueueSet{
            .graphic_queue = *graphics_queue,
            .presentation_queue = *presentation_queue,
            .compute_queue = *compute_queue,
        };
    }

    return std::nullopt;
}

static vk::PhysicalDevice pickPhysicalDevice(vk::Instance instance, vk::SurfaceKHR surface) {
    LOG_INFO(logger, "initializing vulkan physical device...");

    const auto phys_devices = instance.enumeratePhysicalDevices();

    std::vector<std::pair<int, int>> score_index_pair;
    // decide device priority
    for (int i = 0; i < phys_devices.size(); i++) {
        int score = 0;
        const auto &phys_device = phys_devices[i];

        {
            // evaluate queue
            const auto queue_families = phys_device.getQueueFamilyProperties();
            const auto queue_set = pickQueues(phys_device, queue_families, surface);

            if (!queue_set)
                continue;
            if (queue_set->graphic_queue == queue_set->presentation_queue)
                score += 100;
        }
        {
            // evaluate extension
            const auto supported_exts = phys_device.enumerateDeviceExtensionProperties();
            std::vector<std::string> supported_exts_names;
            std::vector<const char *> required_exts_names = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

            for (const auto ext : supported_exts) {
                supported_exts_names.push_back(ext.extensionName.data());
            }
        }

        score_index_pair.push_back({score, i});
    }

    std::stable_sort(score_index_pair.rbegin(), score_index_pair.rend());
    const auto choice_index = score_index_pair[0].second;

    return phys_devices[choice_index];
}

static vk::UniqueDevice createLogicalDevice(vk::PhysicalDevice phys_device, const QueueSet &queues_info) {
    LOG_INFO(logger, "initializing vulkan device...");

    std::vector<const char *> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    vk::DeviceQueueCreateInfo graphics_queue_info, presentation_queue_info, compute_queue_info;

    std::vector<vk::DeviceQueueCreateInfo> queues;
    {
        std::set<uint32_t> queue_indices = {queues_info.graphic_queue, queues_info.presentation_queue,
                                            queues_info.compute_queue};
        for (const auto index : queue_indices) {
            vk::DeviceQueueCreateInfo queue_create_info;
            float queue_priority = 1.0f;
            queue_create_info.queueFamilyIndex = index;
            queue_create_info.setQueuePriorities(queue_priority);
            queues.push_back(queue_create_info);
        }
    }

    vk::DeviceCreateInfo create_info;

    create_info.setPEnabledExtensionNames(exts);
    create_info.setQueueCreateInfos(queues);

    vk::StructureChain create_info_chain{
        create_info,
        vk::PhysicalDeviceDynamicRenderingFeatures{VK_TRUE}, // necessary for dynamic rendering
    };

    return phys_device.createDeviceUnique(create_info_chain.get<vk::DeviceCreateInfo>());
}

static vk::UniqueCommandPool createCommandPool(vk::Device device, uint32_t queue_family_index) {
    LOG_INFO(logger, "creating vulkan command pool...");

    vk::CommandPoolCreateInfo create_info;
    create_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    create_info.queueFamilyIndex = queue_family_index;

    return device.createCommandPoolUnique(create_info);
}

VulkanManageCore::VulkanManageCore(DependencyContainer &container)
    : instance{vulkanCreateInstance(container.get<Window>())},
      surface{container.get<Window>().getVulkanSurface(instance.get())},
      phys_device{pickPhysicalDevice(instance.get(), surface.get())},
      queue_set{pickQueues(phys_device, phys_device.getQueueFamilyProperties(), surface.get()).value()},
      device{createLogicalDevice(phys_device, queue_set)},
      graphic_queue{device->getQueue(queue_set.graphic_queue, 0)}, // queues
      presen_queue{device->getQueue(queue_set.presentation_queue, 0)},
      compute_queue{device->getQueue(queue_set.compute_queue, 0)},
      graphic_cmd_pool{createCommandPool(device.get(), queue_set.graphic_queue)}, // command pools
      compute_cmd_pool{createCommandPool(device.get(), queue_set.compute_queue)} {
    LOG_INFO(logger, "vulkan core initialized");
}
VulkanManageCore::~VulkanManageCore() {}

std::vector<CommandBufWrapper> VulkanManageCore::allocCmdBufs(size_t num) const {
    vk::CommandBufferAllocateInfo alloc_info;
    alloc_info.commandPool = graphic_cmd_pool.get();
    alloc_info.level = vk::CommandBufferLevel::ePrimary;
    alloc_info.commandBufferCount = static_cast<uint32_t>(num);
    auto raw_cmd_bufs = device->allocateCommandBuffersUnique(alloc_info);

    vk::FenceCreateInfo fence_create_info;
    fence_create_info.flags = vk::FenceCreateFlagBits::eSignaled;

    std::vector<CommandBufWrapper> cmd_bufs;
    cmd_bufs.reserve(num);
    for (int i = 0; i < num; i++) {
        cmd_bufs.emplace_back(device.get(), graphic_queue, std::move(raw_cmd_bufs[i]),
                              device->createFenceUnique(fence_create_info));
    }
    assert(cmd_bufs.size() == num);

    return cmd_bufs;
}

std::vector<vk::UniqueSemaphore> VulkanManageCore::createSemaphores(size_t num) const {
    std::vector<vk::UniqueSemaphore> semaphores;
    semaphores.reserve(num);

    vk::SemaphoreCreateInfo create_info;
    for (int i = 0; i < num; i++) {
        semaphores.emplace_back(device->createSemaphoreUnique(create_info));
    }

    return semaphores;
}

} // namespace Pelican
