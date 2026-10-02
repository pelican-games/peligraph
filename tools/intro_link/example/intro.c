/*
 * intro.c -- the smallest Vulkan intro that uses pelican_intro_link.h.
 *
 * Development form (target intro_link_example_dev):
 *     pelican_cli intro-link serve <project>          (prints "port <N>")
 *     intro_link_example_dev [port] [--frames N] [--shot out.bmp]
 *   Tracks and SPIR-V come from the server; a changed shader rebuilds the
 *   pipeline; the server's play / pause / seek drive the clock.
 *
 * Release form (target intro_link_example, PELICAN_INTRO_LINK_BAKED):
 *   the same code over the header `pelican_cli intro-link bake` wrote, with no
 *   C runtime: own entry point, ExitProcess, memset / memcpy written here.
 *
 * Both: a Win32 window and Vulkan 1.3 with dynamic rendering, one full-screen
 * triangle, push constants only (no vertex buffer, descriptor, depth or memory
 * allocation; the triangle is drawn straight into the swapchain image). Only
 * core and WSI functions, imported from vulkan-1.lib.
 *
 * Intro shortcut: no validation, no device selection, no swapchain
 * re-creation and no error checks -- the first physical device is used and
 * queue family 0 is assumed to do graphics and present; the surface is not
 * asked for its formats or capabilities (B8G8R8A8_UNORM, 3 images, the
 * window's fixed 1280x720).
 *
 * Written for size (IL-3): the window is of the system "static" class, the
 * create-info structs of a function share one zeroing, and the release form
 * destroys nothing before ExitProcess.
 */
#ifdef PELICAN_INTRO_LINK_BAKED
#include "intro_baked.h"
#else
#define _CRT_SECURE_NO_WARNINGS
#define PELICAN_INTRO_LINK_IMPLEMENTATION
#endif
#include "pelican_intro_link.h" /* before <windows.h>: the development form brings winsock2.h */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#ifdef PELICAN_INTRO_LINK_BAKED
#include <intrin.h>
/* No C runtime: the compiler still expects these. */
int _fltused = 0;
void *memset(void *dst, int value, size_t size);
void *memcpy(void *dst, const void *src, size_t size);
#pragma function(memset)
#pragma function(memcpy)
void *memset(void *dst, int value, size_t size) {
    __stosb((unsigned char *)dst, (unsigned char)value, size);
    return dst;
}
void *memcpy(void *dst, const void *src, size_t size) {
    __movsb((unsigned char *)dst, (const unsigned char *)src, size);
    return dst;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#define WIDTH 1280
#define HEIGHT 720
#define INTRO_SECONDS 8.0f /* the last key of the project's tracks */
#define MAX_IMAGES 8

/* intro.frag reads these as one vec4. The window is a fixed 1280x720, so the
 * shader works from gl_FragCoord and needs no aspect ratio. */
typedef struct Push {
    float time;
    float track[3]; /* fade, pulse, scene */
} Push;

/* Fixed rather than asked for (intro shortcut): B8G8R8A8_UNORM is what Windows
 * drivers list for a window, and the window is never resized. */
static const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
static const VkExtent2D extent = {WIDTH, HEIGHT};
static VkDevice device;
static VkPipelineLayout layout;
static VkPipeline pipeline;

/* This module's handle without GetModuleHandle: the linker defines __ImageBase. */
extern IMAGE_DOS_HEADER __ImageBase;
#define MODULE_HANDLE ((HINSTANCE)&__ImageBase)

#ifndef PELICAN_INTRO_LINK_BAKED
static int frame_limit;         /* --frames N: stop after N frames */
static const char *shot_path;   /* --shot path.bmp: read the last frame back */
static int rebuilds;
#endif

static VkShaderModule shader_module(const uint32_t *words, uint32_t word_count) {
    VkShaderModuleCreateInfo info = {0};
    VkShaderModule module;
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = (size_t)word_count * 4;
    info.pCode = words;
    vkCreateShaderModule(device, &info, 0, &module);
    return module;
}

/* The development form destroys the modules right after the pipeline is made,
 * so a rebuild only has the old pipeline to destroy. The release form makes
 * the pipeline once and leaves everything to ExitProcess. */
static void create_pipeline(void) {
    struct {
        VkPipelineShaderStageCreateInfo stages[2];
        VkPipelineVertexInputStateCreateInfo vertex_input;
        VkPipelineInputAssemblyStateCreateInfo input_assembly;
        VkViewport viewport;
        VkRect2D scissor;
        VkPipelineViewportStateCreateInfo viewport_state;
        VkPipelineRasterizationStateCreateInfo raster;
        VkPipelineMultisampleStateCreateInfo multisample;
        VkPipelineColorBlendAttachmentState blend_attachment;
        VkPipelineColorBlendStateCreateInfo blend;
        VkPipelineRenderingCreateInfo rendering;
        VkGraphicsPipelineCreateInfo info;
    } z = {0}; /* one zeroing for all of them */
    uint32_t vert_words, frag_words;
    const uint32_t *vert = PIL_SHADER(intro_vert, &vert_words);
    const uint32_t *frag = PIL_SHADER(intro_frag, &frag_words);

    z.stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    z.stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    z.stages[0].module = shader_module(vert, vert_words);
    z.stages[0].pName = "main";
    z.stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    z.stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    z.stages[1].module = shader_module(frag, frag_words);
    z.stages[1].pName = "main";
    z.vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    z.input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    z.input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    z.viewport.width = (float)extent.width;
    z.viewport.height = (float)extent.height;
    z.viewport.maxDepth = 1.0f;
    z.scissor.extent = extent;
    z.viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    z.viewport_state.viewportCount = 1;
    z.viewport_state.pViewports = &z.viewport;
    z.viewport_state.scissorCount = 1;
    z.viewport_state.pScissors = &z.scissor;
    z.raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    z.raster.lineWidth = 1.0f;
    z.multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    z.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    z.blend_attachment.colorWriteMask = 0xF;
    z.blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    z.blend.attachmentCount = 1;
    z.blend.pAttachments = &z.blend_attachment;
    z.rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    z.rendering.colorAttachmentCount = 1;
    z.rendering.pColorAttachmentFormats = &format;
    z.info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    z.info.pNext = &z.rendering;
    z.info.stageCount = 2;
    z.info.pStages = z.stages;
    z.info.pVertexInputState = &z.vertex_input;
    z.info.pInputAssemblyState = &z.input_assembly;
    z.info.pViewportState = &z.viewport_state;
    z.info.pRasterizationState = &z.raster;
    z.info.pMultisampleState = &z.multisample;
    z.info.pColorBlendState = &z.blend;
    z.info.layout = layout;
    vkCreateGraphicsPipelines(device, 0, 1, &z.info, 0, &pipeline);
#ifndef PELICAN_INTRO_LINK_BAKED
    vkDestroyShaderModule(device, z.stages[0].module, 0);
    vkDestroyShaderModule(device, z.stages[1].module, 0);
#endif
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, 0, 0, 0, 1, &barrier);
}

#ifndef PELICAN_INTRO_LINK_BAKED
/* --shot: a buffer the last frame is copied into, written as a 32-bit BMP. */
static VkBuffer shot_buffer;
static unsigned char *shot_pixels;

static void create_shot_buffer(VkPhysicalDevice physical) {
    VkBufferCreateInfo info = {0};
    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties properties;
    VkMemoryAllocateInfo allocate = {0};
    VkDeviceMemory memory;
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t i;
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = (VkDeviceSize)extent.width * extent.height * 4;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(device, &info, 0, &shot_buffer);
    vkGetBufferMemoryRequirements(device, shot_buffer, &requirements);
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
            break;
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = i;
    vkAllocateMemory(device, &allocate, 0, &memory);
    vkBindBufferMemory(device, shot_buffer, memory, 0);
    vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&shot_pixels);
}

static void record_shot(VkCommandBuffer cmd, VkImage image) {
    VkBufferImageCopy region = {0};
    VkMemoryBarrier host = {0};
    image_barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = extent.width;
    region.imageExtent.height = extent.height;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_buffer, 1, &region);
    host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, 0, 0, 0);
    image_barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}

static int write_shot(void) {
    BITMAPFILEHEADER file = {0};
    BITMAPINFOHEADER info = {0};
    const size_t bytes = (size_t)extent.width * extent.height * 4; /* B8G8R8A8: already BMP's order */
    FILE *out;
    file.bfType = 0x4D42; /* "BM" */
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = (DWORD)(file.bfOffBits + bytes);
    info.biSize = sizeof(info);
    info.biWidth = (LONG)extent.width;
    info.biHeight = -(LONG)extent.height; /* top-down rows */
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    out = fopen(shot_path, "wb");
    if (!out) return 0;
    fwrite(&file, sizeof(file), 1, out);
    fwrite(&info, sizeof(info), 1, out);
    fwrite(shot_pixels, 1, bytes, out);
    fclose(out);
    return 1;
}

/* Prints a shader's compile error once each time a new one arrives. */
static void report_shader_error(const char *name, const char *error, const char **last) {
    if (error && error != *last) fprintf(stderr, "intro: %s did not compile; keeping the last pipeline\n%s", name, error);
    *last = error;
}
#endif

static void intro(int show_command) {
    struct {
        VkApplicationInfo application;
        VkInstanceCreateInfo instance_info;
        VkWin32SurfaceCreateInfoKHR surface_info;
        VkDeviceQueueCreateInfo queue_info;
        VkPhysicalDeviceVulkan13Features features13;
        VkDeviceCreateInfo device_info;
        VkSwapchainCreateInfoKHR swapchain_info;
        VkPushConstantRange push_range;
        VkPipelineLayoutCreateInfo layout_info;
        VkCommandPoolCreateInfo pool_info;
        VkCommandBufferAllocateInfo command_info;
        VkFenceCreateInfo fence_info;
        VkSemaphoreCreateInfo semaphore_info;
        VkImageViewCreateInfo view_info;
        VkCommandBufferBeginInfo begin;
        VkRenderingAttachmentInfo attachment;
        VkRenderingInfo rendering;
        VkSubmitInfo submit;
        VkPresentInfoKHR present;
    } z = {0}; /* one zeroing for all of them */
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect = {0, 0, WIDTH, HEIGHT};
    HWND window;
    const char *instance_extensions[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const char *device_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    const float priority = 1.0f;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkSurfaceKHR surface;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkImage images[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    VkSemaphore rendered[MAX_IMAGES]; /* one per image: present may still wait on the last one */
    VkSemaphore acquired;
    VkFence fence;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    uint32_t count, i;
    LARGE_INTEGER frequency, last;
    uint32_t index;
    Push push;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    float time = 0.0f;
    int frame = 0, quit = 0;
#ifndef PELICAN_INTRO_LINK_BAKED
    const char *last_vert_error = 0, *last_frag_error = 0;
#endif

    /* A window of the system "static" class: no class to register and no window
     * procedure. ESC is read off the message queue below; closing the window
     * destroys it, the next acquire fails and the loop ends.
     * WS_VISIBLE with x = CW_USEDEFAULT: Windows shows the window with y as the
     * show command, the same first ShowWindow (which honours the caller's
     * STARTUPINFO) that a separate call would make. */
    AdjustWindowRect(&rect, style, FALSE);
    window = CreateWindowExA(0, "static", "pelican intro link example", style | WS_VISIBLE, CW_USEDEFAULT,
                             show_command, rect.right - rect.left, rect.bottom - rect.top, 0, 0, MODULE_HANDLE, 0);

    z.application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    z.application.apiVersion = VK_API_VERSION_1_3;
    z.instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    z.instance_info.pApplicationInfo = &z.application;
    z.instance_info.enabledExtensionCount = 2;
    z.instance_info.ppEnabledExtensionNames = instance_extensions;
    vkCreateInstance(&z.instance_info, 0, &instance);
    count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    z.surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    z.surface_info.hinstance = MODULE_HANDLE;
    z.surface_info.hwnd = window;
    vkCreateWin32SurfaceKHR(instance, &z.surface_info, 0, &surface);

    z.queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    z.queue_info.queueCount = 1;
    z.queue_info.pQueuePriorities = &priority;
    z.features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    z.features13.dynamicRendering = VK_TRUE;
    z.device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    z.device_info.pNext = &z.features13;
    z.device_info.queueCreateInfoCount = 1;
    z.device_info.pQueueCreateInfos = &z.queue_info;
    z.device_info.enabledExtensionCount = 1;
    z.device_info.ppEnabledExtensionNames = &device_extension;
    vkCreateDevice(physical, &z.device_info, 0, &device);
    vkGetDeviceQueue(device, 0, 0, &queue);

    /* The surface is not asked for its formats or capabilities: three images,
     * sRGB-nonlinear colour space (0), the window's own size, no transform. */
    z.swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    z.swapchain_info.surface = surface;
    z.swapchain_info.minImageCount = 3;
    z.swapchain_info.imageFormat = format;
    z.swapchain_info.imageExtent = extent;
    z.swapchain_info.imageArrayLayers = 1;
    z.swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
#ifndef PELICAN_INTRO_LINK_BAKED
    if (shot_path) z.swapchain_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif
    z.swapchain_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    z.swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    z.swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    z.swapchain_info.clipped = VK_TRUE;
    vkCreateSwapchainKHR(device, &z.swapchain_info, 0, &swapchain);
    count = MAX_IMAGES;
    vkGetSwapchainImagesKHR(device, swapchain, &count, images);
    z.semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    z.view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    z.view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    z.view_info.format = format;
    z.view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    z.view_info.subresourceRange.levelCount = 1;
    z.view_info.subresourceRange.layerCount = 1;
    for (i = 0; i < count; ++i) {
        z.view_info.image = images[i];
        vkCreateImageView(device, &z.view_info, 0, &views[i]);
        vkCreateSemaphore(device, &z.semaphore_info, 0, &rendered[i]);
    }
    vkCreateSemaphore(device, &z.semaphore_info, 0, &acquired);
    z.fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    z.fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &z.fence_info, 0, &fence);
    z.pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    z.pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device, &z.pool_info, 0, &pool);
    z.command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    z.command_info.commandPool = pool;
    z.command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    z.command_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &z.command_info, &cmd);

    z.push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    z.push_range.size = sizeof(Push);
    z.layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    z.layout_info.pushConstantRangeCount = 1;
    z.layout_info.pPushConstantRanges = &z.push_range;
    vkCreatePipelineLayout(device, &z.layout_info, 0, &layout);
    create_pipeline();
#ifndef PELICAN_INTRO_LINK_BAKED
    if (shot_path) create_shot_buffer(physical);
#endif

    /* What every frame submits; only the image index changes. */
    z.begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    z.begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    z.attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    z.attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    z.attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    z.attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    z.rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    z.rendering.renderArea.extent = extent;
    z.rendering.layerCount = 1;
    z.rendering.colorAttachmentCount = 1;
    z.rendering.pColorAttachments = &z.attachment;
    z.submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    z.submit.waitSemaphoreCount = 1;
    z.submit.pWaitSemaphores = &acquired;
    z.submit.pWaitDstStageMask = &wait_stage;
    z.submit.commandBufferCount = 1;
    z.submit.pCommandBuffers = &cmd;
    z.submit.signalSemaphoreCount = 1;
    z.present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    z.present.waitSemaphoreCount = 1;
    z.present.swapchainCount = 1;
    z.present.pSwapchains = &swapchain;
    z.present.pImageIndices = &index;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last);
    while (!quit) {
        MSG message;
        LARGE_INTEGER now;
        float seek_time = 0.0f;
        int paused = 0, held;

        while (PeekMessageA(&message, 0, 0, 0, PM_REMOVE)) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) quit = 1;
            DispatchMessageA(&message);
        }
        pil_poll();
        QueryPerformanceCounter(&now);
        held = pil_time(&seek_time, &paused);
        if (held == 2) time = seek_time;
        else if (!paused) time += (float)(now.QuadPart - last.QuadPart) / (float)frequency.QuadPart;
        last = now;
        if (!held && time >= INTRO_SECONDS) break; /* the server holding the clock keeps the intro open */

#ifndef PELICAN_INTRO_LINK_BAKED
        report_shader_error("intro_vert", PIL_SHADER_ERROR(intro_vert), &last_vert_error);
        report_shader_error("intro_frag", PIL_SHADER_ERROR(intro_frag), &last_frag_error);
        if (PIL_SHADER_CHANGED(intro_vert) | PIL_SHADER_CHANGED(intro_frag)) {
            vkDeviceWaitIdle(device);
            vkDestroyPipeline(device, pipeline, 0);
            create_pipeline();
            fprintf(stderr, "intro: pipeline rebuilt (%d) at frame %d\n", ++rebuilds, frame);
        }
#endif

        push.time = time;
        push.track[0] = PIL_TRACK(fade, time);
        push.track[1] = PIL_TRACK(pulse, time);
        push.track[2] = PIL_TRACK(scene, time);

        vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull);
        vkResetFences(device, 1, &fence);
        if (vkAcquireNextImageKHR(device, swapchain, ~0ull, acquired, 0, &index) < 0) break; /* window closed */
        vkBeginCommandBuffer(cmd, &z.begin);
        image_barrier(cmd, images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        z.attachment.imageView = views[index];
        vkCmdBeginRendering(cmd, &z.rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
#ifndef PELICAN_INTRO_LINK_BAKED
        if (shot_path && frame + 1 == frame_limit) record_shot(cmd, images[index]);
#endif
        image_barrier(cmd, images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(cmd);
        z.submit.pSignalSemaphores = z.present.pWaitSemaphores = &rendered[index];
        vkQueueSubmit(queue, 1, &z.submit, fence);
        vkQueuePresentKHR(queue, &z.present);
        ++frame;
#ifndef PELICAN_INTRO_LINK_BAKED
        if (frame == 1) fprintf(stderr, "intro: first frame presented\n");
        if (frame_limit && frame >= frame_limit) break;
#endif
    }
#ifndef PELICAN_INTRO_LINK_BAKED
    vkDeviceWaitIdle(device); /* the release form ends in ExitProcess with the last frame in flight */
    fprintf(stderr, "intro: %d frames, %d pipeline rebuilds, %ux%u format %d\n", frame, rebuilds, extent.width,
            extent.height, (int)format);
    if (shot_path) {
        if (frame == frame_limit && write_shot()) fprintf(stderr, "intro: wrote %s\n", shot_path);
        else fprintf(stderr, "intro: could not write %s\n", shot_path);
    }
#endif
}

#ifdef PELICAN_INTRO_LINK_BAKED
/* The entry point (/ENTRY:entry); there is no C runtime to return to. */
void entry(void) {
    intro(SW_SHOWDEFAULT);
    ExitProcess(0);
}
#else
int main(int argc, char **argv) {
    int port = 47611, i;
    uint32_t words;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frame_limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot_path = argv[++i];
        else port = atoi(argv[i]);
    }
    if (shot_path && frame_limit <= 0) {
        fprintf(stderr, "intro: --shot needs --frames N (the last frame is the one read back)\n");
        return 2;
    }
    if (!pil_connect("127.0.0.1", port, 5000)) {
        fprintf(stderr, "intro: nothing answered on 127.0.0.1:%d; start `pelican_cli intro-link serve <project>` first\n",
                port);
        return 1;
    }
    if (!PIL_SHADER(intro_vert, &words) || !PIL_SHADER(intro_frag, &words)) {
        const char *vert_error = PIL_SHADER_ERROR(intro_vert), *frag_error = PIL_SHADER_ERROR(intro_frag);
        fprintf(stderr, "intro: the server has no SPIR-V for intro_vert / intro_frag\n%s%s", vert_error ? vert_error : "",
                frag_error ? frag_error : "");
        pil_disconnect();
        return 1;
    }
    /* --frames is the off-screen check: show the window without taking the focus */
    intro(frame_limit ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT);
    pil_disconnect();
    return 0;
}
#endif
