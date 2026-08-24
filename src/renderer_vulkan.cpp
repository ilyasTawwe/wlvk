#include "renderer_vulkan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";
void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed (VkResult "
                                 + std::to_string(static_cast<int>(result)) + ")");
    }
}

const bool g_trace = std::getenv("CODOTAKU_TRACE") != nullptr;

void trace(const std::string& message) {
    if (g_trace) {
        std::fprintf(stderr, "trace: %s\n", message.c_str());
    }
}

// h in [0,1), s and v in [0,1].
void hsv_to_rgb(float h, float s, float v, float out[3]) {
    const float sector = std::floor(h * 6.0f);
    const float f = h * 6.0f - sector;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(sector) % 6) {
    case 0: out[0] = v; out[1] = t; out[2] = p; break;
    case 1: out[0] = q; out[1] = v; out[2] = p; break;
    case 2: out[0] = p; out[1] = v; out[2] = t; break;
    case 3: out[0] = p; out[1] = q; out[2] = v; break;
    case 4: out[0] = t; out[1] = p; out[2] = v; break;
    default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

} // namespace

Renderer::Renderer(wl_display* display, wl_surface* surface, uint32_t width, uint32_t height)
    : display_(display), wayland_surface_(surface), requested_width_(width),
      requested_height_(height) {
    check(volkInitialize(), "volkInitialize");

    create_instance();
    create_surface();
    pick_physical_device();
    create_device();
    create_swapchain(VK_NULL_HANDLE);
    create_image_views();
    create_command_buffers();
    create_sync_objects();

    std::printf("gpu: %s\n", gpu_name_.c_str());
}

Renderer::~Renderer() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    for (VkSemaphore semaphore : finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (VkSemaphore semaphore : available_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (VkFence fence : in_flight_fences_) {
        vkDestroyFence(device_, fence, nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
    }
    for (VkImageView view : swapchain_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (debug_messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void Renderer::create_instance() {
#ifdef CODOTAKU_ENABLE_VALIDATION
    {
        uint32_t count = 0;
        check(vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "vkEnumerateInstanceLayerProperties");
        bool found = false;
        for (const auto& layer : layers) {
            if (std::strcmp(layer.layerName, VALIDATION_LAYER) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error("validation layer requested but not available");
        }
    }
#endif

    std::vector<const char*> extensions = {
        "VK_KHR_surface",
        "VK_KHR_wayland_surface",
    };
    std::vector<const char*> layers;
#ifdef CODOTAKU_ENABLE_VALIDATION
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    layers.push_back(VALIDATION_LAYER);
#endif

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "codotaku media";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "none";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app_info;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();

#ifdef CODOTAKU_ENABLE_VALIDATION
    VkValidationFeatureEnableEXT enabled_validation[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT validation_features = {};
    validation_features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validation_features.enabledValidationFeatureCount =
        static_cast<uint32_t>(std::size(enabled_validation));
    validation_features.pEnabledValidationFeatures = enabled_validation;
    info.pNext = &validation_features;
#endif

    check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
    // Instance-level pointers only; device pointers are loaded by volkLoadDevice.
    volkLoadInstanceOnly(instance_);

#ifdef CODOTAKU_ENABLE_VALIDATION
    VkDebugUtilsMessengerCreateInfoEXT messenger_info = {};
    messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                     | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = &Renderer::debug_callback;

    check(vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &debug_messenger_),
          "vkCreateDebugUtilsMessengerEXT");
#endif
}

VKAPI_ATTR VkBool32 VKAPI_CALL Renderer::debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::fprintf(stderr, "validation [%s]: %s\n",
                 severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                 data->pMessage);
    return VK_FALSE;
}

void Renderer::create_surface() {
    VkWaylandSurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = display_;
    info.surface = wayland_surface_;

    check(vkCreateWaylandSurfaceKHR(instance_, &info, nullptr, &surface_), "vkCreateWaylandSurfaceKHR");
}

void Renderer::pick_physical_device() {
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (count == 0) {
        throw std::runtime_error("no Vulkan-capable physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");

    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(candidate, &props);

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        QueueFamilies indices;
        for (uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && !indices.graphics.has_value()) {
                indices.graphics = i;
            }
            VkBool32 present_supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present_supported);
            if (present_supported == VK_TRUE && !indices.present.has_value()) {
                indices.present = i;
            }
        }
        if (!indices.complete()) {
            continue;
        }

        int score = 0;
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 1000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 500; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 200; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 100; break;
        default: score = 0; break;
        }

        if (score > best_score) {
            best_score = score;
            physical_device_ = candidate;
            queue_families_ = indices;
            gpu_name_ = props.deviceName;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("no physical device with graphics+present support found");
    }
}

void Renderer::create_device() {
    const float priority = 1.0f;

    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    std::vector<uint32_t> unique_families;
    if (queue_families_.graphics == queue_families_.present) {
        unique_families.push_back(queue_families_.graphics.value());
    } else {
        unique_families.push_back(queue_families_.graphics.value());
        unique_families.push_back(queue_families_.present.value());
    }
    for (uint32_t family : unique_families) {
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        queue_infos.push_back(queue_info);
    }

    const char* device_extensions[] = { "VK_KHR_swapchain" };

    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    info.pQueueCreateInfos = queue_infos.data();
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = device_extensions;

    check(vkCreateDevice(physical_device_, &info, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);

    vkGetDeviceQueue(device_, queue_families_.graphics.value(), 0, &graphics_queue_);
    vkGetDeviceQueue(device_, queue_families_.present.value(), 0, &present_queue_);
}

void Renderer::create_swapchain(VkSwapchainKHR old_swapchain) {
    VkSurfaceCapabilitiesKHR caps = {};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t format_count = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data()),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");

    VkSurfaceFormatKHR chosen_format = formats[0];
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB
            && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = format;
            break;
        }
    }

    uint32_t mode_count = 0;
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, nullptr),
          "vkGetPhysicalDeviceSurfacePresentModesKHR");
    std::vector<VkPresentModeKHR> modes(mode_count);
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, modes.data()),
          "vkGetPhysicalDeviceSurfacePresentModesKHR");
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR; // the only guaranteed mode
    for (const VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        }
    }

    VkExtent2D extent = {};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(requested_width_, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(requested_height_, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = chosen_format.format;
    info.imageColorSpace = chosen_format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    // TRANSFER_DST is required by vkCmdClearColorImage.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old_swapchain;

    if (queue_families_.graphics == queue_families_.present) {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    } else {
        const uint32_t families[] = { queue_families_.graphics.value(), queue_families_.present.value() };
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    }

    check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
    }

    swapchain_format_ = chosen_format.format;
    swapchain_extent_ = extent;

    trace("swapchain created: currentExtent=" + std::to_string(caps.currentExtent.width) + "x"
          + std::to_string(caps.currentExtent.height) + (caps.currentExtent.width == UINT32_MAX ? "(MAX)" : "")
          + " chosen=" + std::to_string(extent.width) + "x" + std::to_string(extent.height)
          + " images=" + std::to_string(image_count)
          + " mode=" + std::to_string(static_cast<int>(present_mode))
          + " minRequired=" + std::to_string(caps.minImageExtent.width) + "x" + std::to_string(caps.minImageExtent.height)
          + ".." + std::to_string(caps.maxImageExtent.width) + "x" + std::to_string(caps.maxImageExtent.height));

    uint32_t actual_image_count = 0;
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &actual_image_count, nullptr),
          "vkGetSwapchainImagesKHR");
    swapchain_images_.resize(actual_image_count);
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &actual_image_count, swapchain_images_.data()),
          "vkGetSwapchainImagesKHR");
}

void Renderer::create_image_views() {
    swapchain_views_.resize(swapchain_images_.size());
    for (size_t i = 0; i < swapchain_images_.size(); ++i) {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = swapchain_images_[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = swapchain_format_;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &info, nullptr, &swapchain_views_[i]), "vkCreateImageView");
    }
}

void Renderer::create_command_buffers() {
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_families_.graphics.value();
    check(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool");

    command_buffers_.resize(swapchain_images_.size());
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());
    check(vkAllocateCommandBuffers(device_, &alloc_info, command_buffers_.data()),
          "vkAllocateCommandBuffers");
}

void Renderer::create_sync_objects() {
    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    finished_semaphores_.resize(swapchain_images_.size());
    for (VkSemaphore& semaphore : finished_semaphores_) {
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore), "vkCreateSemaphore");
    }
    image_in_flight_.assign(swapchain_images_.size(), VK_NULL_HANDLE);

    available_semaphores_.resize(MAX_FRAMES_IN_FLIGHT);
    in_flight_fences_.resize(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &available_semaphores_[i]),
              "vkCreateSemaphore");
        check(vkCreateFence(device_, &fence_info, nullptr, &in_flight_fences_[i]), "vkCreateFence");
    }
}

void Renderer::recreate_swapchain() {
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");

    vkDestroyCommandPool(device_, command_pool_, nullptr);
    command_pool_ = VK_NULL_HANDLE;
    command_buffers_.clear();
    for (VkImageView view : swapchain_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_views_.clear();
    swapchain_images_.clear();

    VkSwapchainKHR old_swapchain = swapchain_;
    swapchain_ = VK_NULL_HANDLE;
    create_swapchain(old_swapchain);
    create_image_views();
    create_command_buffers();

    if (finished_semaphores_.size() != swapchain_images_.size()) {
        for (VkSemaphore semaphore : finished_semaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        VkSemaphoreCreateInfo semaphore_info = {};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        finished_semaphores_.assign(swapchain_images_.size(), VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : finished_semaphores_) {
            check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore), "vkCreateSemaphore");
        }
    }
    image_in_flight_.assign(swapchain_images_.size(), VK_NULL_HANDLE);
}

void Renderer::record_command_buffer(uint32_t image_index) {
    const float seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();
    const float hue = std::fmod(seconds * 0.05f, 1.0f);

    float rgb[3] = {};
    hsv_to_rgb(hue, 0.65f, 1.0f, rgb);

    VkCommandBuffer cb = command_buffers_[image_index];
    check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    check(vkBeginCommandBuffer(cb, &begin_info), "vkBeginCommandBuffer");

    const VkImage image = swapchain_images_[image_index];

    VkImageMemoryBarrier to_transfer = {};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    // srcStage matches the acquire semaphore's wait stage so the layout
    // transition is ordered after the presentation engine's read of the image.
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_transfer);

    VkClearColorValue clear_value = {};
    clear_value.float32[0] = rgb[0];
    clear_value.float32[1] = rgb[1];
    clear_value.float32[2] = rgb[2];
    clear_value.float32[3] = 1.0f;

    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);

    VkImageMemoryBarrier to_present = to_transfer;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_present);

    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
}

void Renderer::notify_resized(uint32_t width, uint32_t height) {
    requested_width_ = width;
    requested_height_ = height;
    pending_resize_ = true;
}

void Renderer::draw_frame() {
    if (pending_resize_) {
        pending_resize_ = false;
        recreate_swapchain();
    }

    check(vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX),
          "vkWaitForFences");

    uint32_t image_index = 0;
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                            available_semaphores_[current_frame_], VK_NULL_HANDLE,
                                            &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    }
    check(result != VK_SUBOPTIMAL_KHR ? result : VK_SUCCESS, "vkAcquireNextImageKHR");
    trace("acquire image=" + std::to_string(image_index) + " result=" + std::to_string(static_cast<int>(result)));

    // Don't render into an image whose previous frame is still in flight.
    if (image_in_flight_[image_index] != VK_NULL_HANDLE) {
        check(vkWaitForFences(device_, 1, &image_in_flight_[image_index], VK_TRUE, UINT64_MAX),
              "vkWaitForFences(image_in_flight)");
    }
    image_in_flight_[image_index] = in_flight_fences_[current_frame_];

    record_command_buffer(image_index);

    VkSemaphore wait_semaphores[] = { available_semaphores_[current_frame_] };
    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_TRANSFER_BIT };
    VkSemaphore signal_semaphores[] = { finished_semaphores_[image_index] };

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffers_[image_index];
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    check(vkResetFences(device_, 1, &in_flight_fences_[current_frame_]), "vkResetFences");
    check(vkQueueSubmit(graphics_queue_, 1, &submit_info, in_flight_fences_[current_frame_]),
          "vkQueueSubmit");

    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &image_index;

    result = vkQueuePresentKHR(present_queue_, &present_info);
    trace("present result=" + std::to_string(static_cast<int>(result)));
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    } else {
        check(result, "vkQueuePresentKHR");
    }

    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Renderer::wait_idle() const {
    if (device_ != VK_NULL_HANDLE) {
        check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    }
}
