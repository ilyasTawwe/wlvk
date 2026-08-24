#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <unistd.h>

#include <wayland-client.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <volk.h>
#include <vk_mem_alloc.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"

namespace {

constexpr uint32_t BUFFER_COUNT = 3;
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

#define TRACE(msg) do { if (g_trace) trace(msg); } while (false)

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

struct Framebuffer {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    int dma_buf_fd = -1;
    uint32_t pitch = 0;
    wl_buffer* buffer = nullptr;
    VkSemaphore acquire_timeline = VK_NULL_HANDLE;
    uint32_t acquire_syncobj = 0;
    uint32_t release_syncobj = 0;
    int acquire_fd = -1;
    int release_fd = -1;
    wp_linux_drm_syncobj_timeline_v1* wl_acquire_timeline = nullptr;
    wp_linux_drm_syncobj_timeline_v1* wl_release_timeline = nullptr;
    uint64_t counter = 0;
    bool released = false;
};

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void run();

private:
    static void registry_global(void* data, wl_registry* registry, uint32_t name,
                                const char* interface, uint32_t version);
    static void registry_global_remove(void*, wl_registry*, uint32_t) {}
    static void wm_base_ping(void*, xdg_wm_base* wm_base, uint32_t serial);
    static void xdg_surface_configure(void* data, xdg_surface* xdg_surface, uint32_t serial);
    static void toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                                   wl_array*);
    static void toplevel_close(void* data, xdg_toplevel*);
    static void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
    static void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
    static void frame_done(void* data, wl_callback* callback, uint32_t);
    static void dmabuf_format(void*, zwp_linux_dmabuf_v1*, uint32_t) {}
    static void dmabuf_modifier(void*, zwp_linux_dmabuf_v1*, uint32_t, uint32_t, uint32_t) {}

    static void feedback_done(void*, zwp_linux_dmabuf_feedback_v1*) {}
    static void feedback_format_table(void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd,
                                      uint32_t size);
    static void feedback_main_device(void* data, zwp_linux_dmabuf_feedback_v1*,
                                     wl_array* device);
    static void feedback_tranche_done(void* data, zwp_linux_dmabuf_feedback_v1*);
    static void feedback_tranche_target_device(void*, zwp_linux_dmabuf_feedback_v1*,
                                               wl_array*) {}
    static void feedback_tranche_formats(void* data, zwp_linux_dmabuf_feedback_v1*,
                                         wl_array* indices);
    static void feedback_tranche_flags(void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) {}

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data);

    static constexpr wl_registry_listener registry_listener_ = {
        .global = &registry_global,
        .global_remove = &registry_global_remove,
    };
    static constexpr xdg_wm_base_listener wm_base_listener_ = {
        .ping = &wm_base_ping,
    };
    static constexpr xdg_surface_listener xdg_surface_listener_ = {
        .configure = &xdg_surface_configure,
    };
    static constexpr xdg_toplevel_listener toplevel_listener_ = {
        .configure = &toplevel_configure,
        .close = &toplevel_close,
        .configure_bounds = &toplevel_configure_bounds,
        .wm_capabilities = &toplevel_wm_capabilities,
    };
    static constexpr wl_callback_listener frame_listener_ = {
        .done = &frame_done,
    };
    static constexpr zwp_linux_dmabuf_v1_listener dmabuf_listener_ = {
        .format = &dmabuf_format,
        .modifier = &dmabuf_modifier,
    };
    static constexpr zwp_linux_dmabuf_feedback_v1_listener feedback_listener_ = {
        .done = &feedback_done,
        .format_table = &feedback_format_table,
        .main_device = &feedback_main_device,
        .tranche_done = &feedback_tranche_done,
        .tranche_target_device = &feedback_tranche_target_device,
        .tranche_formats = &feedback_tranche_formats,
        .tranche_flags = &feedback_tranche_flags,
    };

    void init_wayland();
    void check_display();
    void create_window();
    void poll_and_dispatch();
    void arm_frame();
    void query_default_feedback();
    void query_supported_modifiers();
    void choose_modifier();

    void init_vulkan();
    void create_instance();
    void pick_physical_device();
    void create_device();
    void create_allocator();
    void open_drm_node();
    uint32_t create_syncobj();
    wp_linux_drm_syncobj_timeline_v1* share_syncobj(uint32_t syncobj, int* out_fd);
    void materialize_acquire_point(uint32_t syncobj, uint64_t point, VkFence fence);
    bool wait_release_point(uint32_t syncobj, uint64_t point, int64_t timeout_ns);
    void create_framebuffers();
    void create_dmabuf_resources();
    void handle_resize();
    void reap_retired();
    void create_command_buffers();

    void present(uint32_t index, float r, float g, float b);
    void record_clear(uint32_t index, float r, float g, float b);

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    xdg_wm_base* wm_base_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    wl_callback* frame_callback_ = nullptr;
    zwp_linux_dmabuf_v1* dmabuf_ = nullptr;
    wp_linux_drm_syncobj_manager_v1* syncobj_manager_ = nullptr;
    wp_linux_drm_syncobj_surface_v1* syncobj_surface_ = nullptr;
    zwp_linux_dmabuf_feedback_v1* default_feedback_ = nullptr;

    std::vector<uint8_t> format_table_;
    std::vector<std::pair<uint32_t, uint64_t>> tranche_candidates_;
    std::vector<std::pair<uint32_t, uint64_t>> feedback_formats_;
    uint64_t chosen_modifier_ = 0;
    std::vector<uint64_t> supported_modifiers_;
    dev_t main_device_ = 0;

    int drm_fd_ = -1;

    bool configured_ = false;
    bool closed_ = false;
    bool frame_due_ = false;
    bool resize_pending_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t pending_width_ = 0;
    uint32_t pending_height_ = 0;

    VkInstance instance_ = VK_NULL_HANDLE;
#ifdef CODOTAKU_ENABLE_VALIDATION
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
#endif

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    uint32_t graphics_family_ = 0;
    uint32_t present_family_ = 0;
    std::string gpu_name_;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VmaPool dmabuf_pool_ = nullptr;
    VkExportMemoryAllocateInfo dmabuf_export_{};
    VkQueue queue_ = VK_NULL_HANDLE;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    std::vector<Framebuffer> frames_;
    std::vector<Framebuffer> retired_;
    uint32_t next_frame_ = 0;

    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

void Application::registry_global(void* data, wl_registry* registry, uint32_t name,
                            const char* interface, uint32_t version) {
    auto* self = static_cast<Application*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0 && self->compositor_ == nullptr) {
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             version < 4 ? version : 4));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0 && self->wm_base_ == nullptr) {
        self->wm_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(self->wm_base_, &wm_base_listener_, self);
    } else if (std::strcmp(interface, "zwp_linux_dmabuf_v1") == 0 && self->dmabuf_ == nullptr) {
        self->dmabuf_ = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
                             version < 4 ? version : 4));
        zwp_linux_dmabuf_v1_add_listener(self->dmabuf_, &dmabuf_listener_, self);
    } else if (std::strcmp(interface, "wp_linux_drm_syncobj_manager_v1") == 0
               && self->syncobj_manager_ == nullptr) {
        self->syncobj_manager_ = static_cast<wp_linux_drm_syncobj_manager_v1*>(
            wl_registry_bind(registry, name, &wp_linux_drm_syncobj_manager_v1_interface, 1));
    }
}

void Application::wm_base_ping(void*, xdg_wm_base* wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

void Application::xdg_surface_configure(void* data, xdg_surface* xdg_surface, uint32_t serial) {
    auto* self = static_cast<Application*>(data);
    xdg_surface_ack_configure(xdg_surface, serial);
    self->configured_ = true;
}

void Application::toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                               wl_array*) {
    auto* self = static_cast<Application*>(data);
    if (width <= 0 || height <= 0) {
        return;
    }
    self->pending_width_ = static_cast<uint32_t>(width);
    self->pending_height_ = static_cast<uint32_t>(height);
    if (self->configured_
        && (self->pending_width_ != self->width_ || self->pending_height_ != self->height_)) {
        self->resize_pending_ = true;
    }
}

void Application::toplevel_close(void* data, xdg_toplevel*) {
    static_cast<Application*>(data)->closed_ = true;
}

void Application::frame_done(void* data, wl_callback* callback, uint32_t) {
    auto* self = static_cast<Application*>(data);
    wl_callback_destroy(callback);
    self->frame_callback_ = nullptr;
    self->frame_due_ = true;
}

#ifdef CODOTAKU_ENABLE_VALIDATION
VKAPI_ATTR VkBool32 VKAPI_CALL Application::debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::fprintf(stderr, "validation [%s]: %s\n",
                 severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                 data->pMessage);
    return VK_FALSE;
}
#endif

Application::Application() {
    init_wayland();
    init_vulkan();
    std::printf("codotaku-media: %ux%u, gpu: %s\n", width_, height_, gpu_name_.c_str());
}

Application::~Application() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    if (!retired_.empty()) {
        for (auto& fb : retired_) {
            if (fb.buffer != nullptr) {
                wl_buffer_destroy(fb.buffer);
            }
            if (fb.image != VK_NULL_HANDLE) {
                vmaDestroyImage(allocator_, fb.image, fb.allocation);
            }
            if (fb.dma_buf_fd >= 0) {
                ::close(fb.dma_buf_fd);
            }
        }
        retired_.clear();
    }
    if (syncobj_surface_ != nullptr) {
        wp_linux_drm_syncobj_surface_v1_destroy(syncobj_surface_);
    }
    for (auto& frame : frames_) {
        if (frame.buffer != nullptr) {
            wl_buffer_destroy(frame.buffer);
        }
        if (frame.wl_acquire_timeline != nullptr) {
            wp_linux_drm_syncobj_timeline_v1_destroy(frame.wl_acquire_timeline);
        }
        if (frame.wl_release_timeline != nullptr) {
            wp_linux_drm_syncobj_timeline_v1_destroy(frame.wl_release_timeline);
        }
        if (frame.acquire_fd >= 0) {
            ::close(frame.acquire_fd);
        }
        if (frame.release_fd >= 0) {
            ::close(frame.release_fd);
        }
        if (drm_fd_ >= 0) {
            if (frame.acquire_syncobj != 0) {
                drmSyncobjDestroy(drm_fd_, frame.acquire_syncobj);
            }
            if (frame.release_syncobj != 0) {
                drmSyncobjDestroy(drm_fd_, frame.release_syncobj);
            }
        }
        if (frame.dma_buf_fd >= 0) {
            ::close(frame.dma_buf_fd);
        }
        if (frame.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, frame.image, frame.allocation);
        }
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
    }
    if (dmabuf_pool_ != nullptr) {
        vmaDestroyPool(allocator_, dmabuf_pool_);
    }
    vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
#if defined(CODOTAKU_ENABLE_VALIDATION)
    if (debug_messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    }
#endif
    vkDestroyInstance(instance_, nullptr);
    if (drm_fd_ >= 0) {
        ::close(drm_fd_);
    }
    if (frame_callback_ != nullptr) {
        wl_callback_destroy(frame_callback_);
    }
    if (toplevel_ != nullptr) {
        xdg_toplevel_destroy(toplevel_);
    }
    if (xdg_surface_ != nullptr) {
        xdg_surface_destroy(xdg_surface_);
    }
    if (wm_base_ != nullptr) {
        xdg_wm_base_destroy(wm_base_);
    }
    if (surface_ != nullptr) {
        wl_surface_destroy(surface_);
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
    }
    if (display_ != nullptr) {
        wl_display_disconnect(display_);
    }
}

void Application::check_display() {
    if (const uint32_t error = wl_display_get_error(display_); error != 0) {
        throw std::runtime_error("wayland display error: " + std::to_string(error));
    }
}

void Application::init_wayland() {
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        throw std::runtime_error("failed to connect to wayland display");
    }
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener_, this);
    if (wl_display_roundtrip(display_) < 0) {
        throw std::runtime_error("wayland roundtrip failed");
    }
    check_display();
    if (compositor_ == nullptr || wm_base_ == nullptr) {
        throw std::runtime_error("missing wl_compositor / xdg_wm_base");
    }
    if (dmabuf_ == nullptr || syncobj_manager_ == nullptr) {
        throw std::runtime_error(
            "compositor lacks linux-dmabuf / linux-drm-syncobj; explicit sync unavailable");
    }
    create_window();
    while (!configured_ && !closed_) {
        if (wl_display_dispatch(display_) < 0) {
            throw std::runtime_error("wayland dispatch failed");
        }
        check_display();
    }
    if (!configured_) {
        throw std::runtime_error("compositor closed window before configure");
    }
    width_ = pending_width_ ? pending_width_ : 1280;
    height_ = pending_height_ ? pending_height_ : 720;
}

void Application::create_window() {
    surface_ = wl_compositor_create_surface(compositor_);
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_surface_add_listener(xdg_surface_, &xdg_surface_listener_, this);
    xdg_toplevel_add_listener(toplevel_, &toplevel_listener_, this);
    xdg_toplevel_set_title(toplevel_, "codotaku-media");
    xdg_toplevel_set_app_id(toplevel_, "codotaku-media");
    wl_surface_commit(surface_);
}

void Application::poll_and_dispatch() {
    if (!frame_due_ && !closed_) {
        if (wl_display_flush(display_) < 0) {
            throw std::runtime_error("wayland flush failed");
        }
        pollfd pfd = {};
        pfd.fd = wl_display_get_fd(display_);
        pfd.events = POLLIN;
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            throw std::runtime_error("poll on wayland fd failed");
        }
    }
    while (wl_display_dispatch_pending(display_) > 0) {
    }
    if (frame_due_ || closed_) {
        return;
    }
    if (wl_display_dispatch(display_) < 0) {
        throw std::runtime_error("wayland dispatch failed");
    }
    while (wl_display_dispatch_pending(display_) > 0) {
    }
    check_display();
}

void Application::arm_frame() {
    frame_callback_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_callback_, &frame_listener_, this);
    if (wl_display_flush(display_) < 0) {
        throw std::runtime_error("wayland flush failed");
    }
}

void Application::create_instance() {
    std::vector<const char*> extensions = {
        "VK_KHR_surface",
        "VK_KHR_wayland_surface",
    };
    std::vector<const char*> layers;
#if defined(CODOTAKU_ENABLE_VALIDATION)
    uint32_t layer_count = 0;
    check(vkEnumerateInstanceLayerProperties(&layer_count, nullptr),
          "vkEnumerateInstanceLayerProperties");
    std::vector<VkLayerProperties> available(layer_count);
    check(vkEnumerateInstanceLayerProperties(&layer_count, available.data()),
          "vkEnumerateInstanceLayerProperties");
    for (const auto& layer : available) {
        if (std::strcmp(layer.layerName, VALIDATION_LAYER) == 0) {
            layers.push_back(VALIDATION_LAYER);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            break;
        }
    }
    if (layers.empty()) {
        throw std::runtime_error("validation requested but layer unavailable");
    }
#endif

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "codotaku-media";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app_info;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
    volkLoadInstanceOnly(instance_);

#if defined(CODOTAKU_ENABLE_VALIDATION)
    VkDebugUtilsMessengerCreateInfoEXT messenger_info = {};
    messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                     | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = &Application::debug_callback;
    check(vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr,
                                         &debug_messenger_),
          "vkCreateDebugUtilsMessengerEXT");
#endif
}

void Application::pick_physical_device() {
    const char* required[] = {
        "VK_KHR_external_semaphore_fd",
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_KHR_external_fence_fd",
        "VK_EXT_image_drm_format_modifier",
    };

    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (count == 0) {
        throw std::runtime_error("no Vulkan devices");
    }
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()),
          "vkEnumeratePhysicalDevices");

    for (VkPhysicalDevice candidate : devices) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        bool have_graphics = false;
        bool have_present = false;
        for (uint32_t i = 0; i < family_count && !(have_graphics && have_present); ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && !have_graphics) {
                graphics_family_ = i;
                have_graphics = true;
            }
            VkBool32 present_ok =
                vkGetPhysicalDeviceWaylandPresentationSupportKHR(candidate, i, display_);
            if (present_ok && !have_present) {
                present_family_ = i;
                have_present = true;
            }
        }
        if (!have_graphics || !have_present) {
            continue;
        }

        uint32_t ext_count = 0;
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ext_count, nullptr),
              "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> exts(ext_count);
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ext_count, exts.data()),
              "vkEnumerateDeviceExtensionProperties");
        bool all_found = true;
        for (const char* want : required) {
            bool found = false;
            for (const auto& ext : exts) {
                if (std::strcmp(ext.extensionName, want) == 0) {
                    found = true;
                    break;
                }
            }
            all_found = all_found && found;
            if (!all_found) {
                break;
            }
        }
        if (!all_found) {
            continue;
        }

        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(candidate, &props);
        physical_device_ = candidate;
        gpu_name_ = props.deviceName;
        return;
    }
    throw std::runtime_error(
        "no device with graphics+present and external memory/semaphore support");
}

void Application::create_device() {
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    std::vector<uint32_t> families;
    if (graphics_family_ == present_family_) {
        families.push_back(graphics_family_);
    } else {
        families.push_back(graphics_family_);
        families.push_back(present_family_);
    }
    for (uint32_t family : families) {
        VkDeviceQueueCreateInfo qi = {};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queue_infos.push_back(qi);
    }

    const char* device_extensions[] = {
        "VK_KHR_external_semaphore_fd",
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_KHR_external_fence_fd",
        "VK_EXT_image_drm_format_modifier",
    };

    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    info.pQueueCreateInfos = queue_infos.data();
    info.enabledExtensionCount = 5;
    info.ppEnabledExtensionNames = device_extensions;
    check(vkCreateDevice(physical_device_, &info, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphics_family_, 0, &queue_);
}

void Application::create_allocator() {
    VmaAllocatorCreateInfo info = {};
    info.instance = instance_;
    info.physicalDevice = physical_device_;
    info.device = device_;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    VmaVulkanFunctions functions = {};
    check(vmaImportVulkanFunctionsFromVolk(&info, &functions),
          "vmaImportVulkanFunctionsFromVolk");
    info.pVulkanFunctions = &functions;
    check(vmaCreateAllocator(&info, &allocator_), "vmaCreateAllocator");
}

void Application::create_framebuffers() {
    frames_.resize(BUFFER_COUNT);

    dmabuf_export_.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    dmabuf_export_.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    const bool linear = chosen_modifier_ == 0;

    VkImageCreateInfo pool_image_info = {};
    pool_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    pool_image_info.imageType = VK_IMAGE_TYPE_2D;
    pool_image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    pool_image_info.extent = {width_, height_, 1};
    pool_image_info.mipLevels = 1;
    pool_image_info.arrayLayers = 1;
    pool_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    pool_image_info.tiling = linear ? VK_IMAGE_TILING_LINEAR
                                    : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    pool_image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    pool_image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    pool_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo pool_alloc_template = {};
    pool_alloc_template.usage = VMA_MEMORY_USAGE_AUTO;
    uint32_t mem_type_index = 0;
    if (linear) {
        check(vmaFindMemoryTypeIndexForImageInfo(allocator_, &pool_image_info,
                                                 &pool_alloc_template, &mem_type_index),
              "vmaFindMemoryTypeIndexForImageInfo");
    } else {
        VkImageCreateInfo query_info = pool_image_info;
        query_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        query_info.pNext = nullptr;
        check(vmaFindMemoryTypeIndexForImageInfo(allocator_, &query_info,
                                                 &pool_alloc_template, &mem_type_index),
              "vmaFindMemoryTypeIndexForImageInfo");
    }

    VmaPoolCreateInfo pool_info = {};
    pool_info.memoryTypeIndex = mem_type_index;
    pool_info.pMemoryAllocateNext = &dmabuf_export_;
    check(vmaCreatePool(allocator_, &pool_info, &dmabuf_pool_), "vmaCreatePool(dmabuf)");

    create_dmabuf_resources();

    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        Framebuffer& fb = frames_[i];

        fb.acquire_syncobj = create_syncobj();
        fb.wl_acquire_timeline = share_syncobj(fb.acquire_syncobj, &fb.acquire_fd);
        if (fb.wl_acquire_timeline == nullptr) {
            throw std::runtime_error("import_timeline(acquire) failed");
        }

        fb.release_syncobj = create_syncobj();
        fb.wl_release_timeline = share_syncobj(fb.release_syncobj, &fb.release_fd);
        if (fb.wl_release_timeline == nullptr) {
            throw std::runtime_error("import_timeline(release) failed");
        }
    }

    if (wl_display_roundtrip(display_) < 0) {
        throw std::runtime_error("wayland roundtrip after timeline import failed");
    }
    for (auto& frame : frames_) {
        ::close(frame.acquire_fd);
        ::close(frame.release_fd);
        frame.acquire_fd = -1;
        frame.release_fd = -1;
    }

    syncobj_surface_ =
        wp_linux_drm_syncobj_manager_v1_get_surface(syncobj_manager_, surface_);
    if (syncobj_surface_ == nullptr) {
        throw std::runtime_error("get_surface(syncobj) failed");
    }

    VkCommandPoolCreateInfo cmd_pool_info = {};
    cmd_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmd_pool_info.queueFamilyIndex = graphics_family_;
    check(vkCreateCommandPool(device_, &cmd_pool_info, nullptr, &command_pool_),
          "vkCreateCommandPool");

    command_buffers_.resize(BUFFER_COUNT);
    VkCommandBufferAllocateInfo cb_alloc = {};
    cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_alloc.commandPool = command_pool_;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = BUFFER_COUNT;
    check(vkAllocateCommandBuffers(device_, &cb_alloc, command_buffers_.data()),
          "vkAllocateCommandBuffers");
}

void Application::create_dmabuf_resources() {
    const bool linear = chosen_modifier_ == 0;

    VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {};
    if (!linear) {
        modifier_list.sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
        modifier_list.drmFormatModifierCount = 1;
        modifier_list.pDrmFormatModifiers = &chosen_modifier_;
    }

    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        Framebuffer& fb = frames_[i];

        VkExternalMemoryImageCreateInfo external_image = {};
        external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (!linear) {
            external_image.pNext = &modifier_list;
        }

        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext = &external_image;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_info.extent = {width_, height_, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = linear ? VK_IMAGE_TILING_LINEAR
                                   : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        alloc_info.pool = dmabuf_pool_;

        check(vmaCreateImage(allocator_, &image_info, &alloc_info, &fb.image, &fb.allocation,
                             nullptr),
              "vmaCreateImage(dmabuf)");

        VmaAllocationInfo alloc_result = {};
        vmaGetAllocationInfo(allocator_, fb.allocation, &alloc_result);

        VkMemoryGetFdInfoKHR get_fd = {};
        get_fd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        get_fd.memory = alloc_result.deviceMemory;
        get_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        check(vkGetMemoryFdKHR(device_, &get_fd, &fb.dma_buf_fd), "vkGetMemoryFdKHR");

        VkImageSubresource subresource = {};
        subresource.aspectMask = linear ? VK_IMAGE_ASPECT_COLOR_BIT
                                        : VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
        VkSubresourceLayout layout = {};
        vkGetImageSubresourceLayout(device_, fb.image, &subresource, &layout);
        fb.pitch = static_cast<uint32_t>(layout.rowPitch);

        zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabuf_);
        zwp_linux_buffer_params_v1_add(params, fb.dma_buf_fd, 0, 0, fb.pitch,
                                       static_cast<uint32_t>(chosen_modifier_ >> 32),
                                       static_cast<uint32_t>(chosen_modifier_ & 0xffffffff));
        fb.buffer = zwp_linux_buffer_params_v1_create_immed(params, width_, height_,
                                                            DRM_FORMAT_XRGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (fb.buffer == nullptr) {
            throw std::runtime_error("zwp_linux_buffer_params_v1_create_immed failed");
        }
        TRACE("image " + std::to_string(i) + ": fd=" + std::to_string(fb.dma_buf_fd)
              + " pitch=" + std::to_string(fb.pitch) + " " + std::to_string(width_) + "x"
              + std::to_string(height_));
    }
}

void Application::handle_resize() {
    TRACE("resize: " + std::to_string(width_) + "x" + std::to_string(height_) + " -> "
              + std::to_string(pending_width_) + "x" + std::to_string(pending_height_));
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(resize)");

    std::vector<Framebuffer> fresh(BUFFER_COUNT);
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        Framebuffer& old = frames_[i];
        Framebuffer& dst = fresh[i];

        dst.acquire_syncobj = old.acquire_syncobj;
        dst.release_syncobj = old.release_syncobj;
        dst.acquire_fd = old.acquire_fd;
        dst.release_fd = old.release_fd;
        dst.wl_acquire_timeline = old.wl_acquire_timeline;
        dst.wl_release_timeline = old.wl_release_timeline;
        dst.counter = old.counter;

        Framebuffer retired;
        retired.image = old.image;
        retired.allocation = old.allocation;
        retired.dma_buf_fd = old.dma_buf_fd;
        retired.pitch = old.pitch;
        retired.buffer = old.buffer;
        retired.release_syncobj = old.release_syncobj;
        retired.counter = old.counter;
        retired_.push_back(retired);

        old.image = VK_NULL_HANDLE;
        old.allocation = nullptr;
        old.dma_buf_fd = -1;
        old.pitch = 0;
        old.buffer = nullptr;
    }
    width_ = pending_width_;
    height_ = pending_height_;
    resize_pending_ = false;

    frames_ = std::move(fresh);
    create_dmabuf_resources();
}

void Application::reap_retired() {
    constexpr int64_t wait_ns = 100LL * 1000LL * 1000LL;
    for (auto it = retired_.begin(); it != retired_.end();) {
        Framebuffer& fb = *it;
        bool ready = true;
        if (!fb.released && fb.counter > 0) {
            ready = wait_release_point(fb.release_syncobj, fb.counter, wait_ns);
            fb.released = ready;
        }
        if (!ready) {
            ++it;
            continue;
        }
        if (fb.buffer != nullptr) {
            wl_buffer_destroy(fb.buffer);
            fb.buffer = nullptr;
        }
        if (fb.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, fb.image, fb.allocation);
            fb.image = VK_NULL_HANDLE;
            fb.allocation = nullptr;
        }
        if (fb.dma_buf_fd >= 0) {
            ::close(fb.dma_buf_fd);
            fb.dma_buf_fd = -1;
        }
        it = retired_.erase(it);
    }
}

void Application::init_vulkan() {
    check(volkInitialize(), "volkInitialize");
    create_instance();
    pick_physical_device();
    query_supported_modifiers();
    query_default_feedback();
    create_device();
    create_allocator();
    open_drm_node();
    create_framebuffers();
}

void Application::open_drm_node() {
    if (main_device_ != 0) {
        drmDevicePtr devices[16] = {};
        const int count = drmGetDevices2(0, devices, 16);
        for (int i = 0; i < count; ++i) {
            const drmDevicePtr dev = devices[i];
            const char* matched = nullptr;
            for (int t = 0; t < DRM_NODE_MAX && matched == nullptr; ++t) {
                if ((dev->available_nodes & (1u << t)) == 0) {
                    continue;
                }
                struct stat st = {};
                if (::stat(dev->nodes[t], &st) == 0 && st.st_rdev == main_device_) {
                    matched = dev->nodes[t];
                }
            }
            if (matched == nullptr) {
                continue;
            }
            const bool have_render = (dev->available_nodes & (1u << DRM_NODE_RENDER)) != 0;
            const char* node = have_render ? dev->nodes[DRM_NODE_RENDER] : matched;
            drm_fd_ = ::open(node, O_RDWR | O_CLOEXEC);
            if (drm_fd_ >= 0) {
                TRACE(std::string("opened drm node ") + node + " (matched "
                      + matched + " via dmabuf main_device)");
                break;
            }
        }
        if (count > 0) {
            drmFreeDevices(devices, count);
        }
    }
    if (drm_fd_ < 0) {
        TRACE(main_device_ != 0 ? "no drm device matched main_device; using fallback node"
                                : "main_device unknown; using fallback node");
        drm_fd_ = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    }
    if (drm_fd_ < 0) {
        throw std::runtime_error("failed to open a usable DRM render node");
    }
}

uint32_t Application::create_syncobj() {
    uint32_t handle = 0;
    if (drmSyncobjCreate(drm_fd_, 0, &handle) != 0) {
        throw std::runtime_error("drmSyncobjCreate failed");
    }
    return handle;
}

wp_linux_drm_syncobj_timeline_v1* Application::share_syncobj(uint32_t syncobj, int* out_fd) {
    int fd = -1;
    if (drmSyncobjHandleToFD(drm_fd_, syncobj, &fd) != 0) {
        throw std::runtime_error("drmSyncobjHandleToFD failed");
    }
    *out_fd = fd;
    wp_linux_drm_syncobj_timeline_v1* timeline =
        wp_linux_drm_syncobj_manager_v1_import_timeline(syncobj_manager_, fd);
    return timeline;
}

void Application::materialize_acquire_point(uint32_t syncobj, uint64_t point, VkFence fence) {
    VkFenceGetFdInfoKHR get_fd_info = {};
    get_fd_info.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
    get_fd_info.fence = fence;
    get_fd_info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    int sync_fd = -1;
    check(vkGetFenceFdKHR(device_, &get_fd_info, &sync_fd), "vkGetFenceFdKHR");
    if (sync_fd < 0) {
        throw std::runtime_error("vkGetFenceFdKHR returned no sync fd");
    }

    uint32_t tmp = 0;
    if (drmSyncobjCreate(drm_fd_, 0, &tmp) != 0
        || drmSyncobjImportSyncFile(drm_fd_, tmp, sync_fd) != 0
        || drmSyncobjTransfer(drm_fd_, syncobj, point, tmp, 0, 0) != 0) {
        ::close(sync_fd);
        if (tmp != 0) {
            drmSyncobjDestroy(drm_fd_, tmp);
        }
        throw std::runtime_error("failed to materialize acquire point from sync fd");
    }
    drmSyncobjDestroy(drm_fd_, tmp);
    ::close(sync_fd);
}

bool Application::wait_release_point(uint32_t syncobj, uint64_t point, int64_t timeout_ns) {
    uint32_t first = 0;
    if (drmSyncobjTimelineWait(drm_fd_, &syncobj, &point, 1, timeout_ns, 0, &first) != 0) {
        TRACE("release point " + std::to_string(point) + " not signaled within timeout");
        return false;
    }
    TRACE("release point " + std::to_string(point) + " signaled");
    return true;
}

void Application::feedback_main_device(void* data, zwp_linux_dmabuf_feedback_v1*,
                                       wl_array* device) {
    auto* self = static_cast<Application*>(data);
    if (device->size >= sizeof(dev_t)) {
        std::memcpy(&self->main_device_, device->data, sizeof(dev_t));
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%lx", static_cast<unsigned long>(self->main_device_));
        TRACE(std::string("dmabuf main_device dev_t=0x") + hex);
    }
}

void Application::feedback_format_table(void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd,
                                  uint32_t size) {    auto* self = static_cast<Application*>(data);
    self->format_table_.resize(size);
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(size)) {
        const ssize_t n = pread(fd, self->format_table_.data() + total, size - total, total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    ::close(fd);
}

void Application::feedback_tranche_formats(void* data, zwp_linux_dmabuf_feedback_v1*,
                                     wl_array* indices) {
    auto* self = static_cast<Application*>(data);
    const auto* idx = static_cast<const uint16_t*>(indices->data);
    const size_t count = indices->size / sizeof(uint16_t);
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = static_cast<size_t>(idx[i]) * 16;
        if (offset + 16 > self->format_table_.size()) {
            continue;
        }
        uint32_t format = 0;
        uint64_t modifier = 0;
        std::memcpy(&format, self->format_table_.data() + offset, 4);
        std::memcpy(&modifier, self->format_table_.data() + offset + 8, 8);
        if (format == DRM_FORMAT_XRGB8888) {
            self->tranche_candidates_.push_back({format, modifier});
        }
    }
}

void Application::feedback_tranche_done(void* data, zwp_linux_dmabuf_feedback_v1*) {
    auto* self = static_cast<Application*>(data);
    if (self->feedback_formats_.empty()) {
        self->feedback_formats_ = self->tranche_candidates_;
    }
    self->tranche_candidates_.clear();
}

void Application::query_default_feedback() {
    default_feedback_ = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf_);
    if (default_feedback_ == nullptr) {
        throw std::runtime_error("get_default_feedback failed");
    }
    zwp_linux_dmabuf_feedback_v1_add_listener(default_feedback_, &feedback_listener_, this);
    if (wl_display_roundtrip(display_) < 0 || wl_display_roundtrip(display_) < 0) {
        throw std::runtime_error("wayland roundtrip during feedback failed");
    }
    choose_modifier();
}

void Application::query_supported_modifiers() {
    VkDrmFormatModifierPropertiesListEXT mod_list = {};
    mod_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

    VkFormatProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props.pNext = &mod_list;
    vkGetPhysicalDeviceFormatProperties2(physical_device_, VK_FORMAT_B8G8R8A8_UNORM, &props);

    if (mod_list.drmFormatModifierCount == 0) {
        TRACE("driver reports no DRM format modifiers; accepting compositor choice blindly");
        return;
    }
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(mod_list.drmFormatModifierCount);
    mod_list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(physical_device_, VK_FORMAT_B8G8R8A8_UNORM, &props);

    for (const auto& m : mods) {
        if ((m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0) {
            supported_modifiers_.push_back(m.drmFormatModifier);
        }
    }
}

void Application::choose_modifier() {
    if (feedback_formats_.empty()) {
        throw std::runtime_error("compositor feedback offers no XRGB8888 dmabuf formats");
    }
    for (size_t i = 0; i < feedback_formats_.size(); ++i) {
        const uint64_t modifier = feedback_formats_[i].second;
        if (!supported_modifiers_.empty()
            && std::find(supported_modifiers_.begin(), supported_modifiers_.end(), modifier)
                   == supported_modifiers_.end()) {
            continue;
        }
        chosen_modifier_ = modifier;
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%llx", static_cast<unsigned long long>(modifier));
        const std::string rank = i == 0
                                     ? " (compositor top pick)"
                                     : " (rank " + std::to_string(i) + " in feedback order)";
        TRACE(std::string("chosen modifier 0x") + hex + rank);
        return;
    }
    throw std::runtime_error("no compositor-offered modifier is Vulkan-supported");
}

void Application::record_clear(uint32_t index, float r, float g, float b) {
    VkCommandBuffer cb = command_buffers_[index];
    check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    check(vkBeginCommandBuffer(cb, &begin), "vkBeginCommandBuffer");

    VkImageMemoryBarrier to_transfer = {};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = frames_[index].image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_transfer);

    VkClearColorValue clear = {};
    clear.float32[0] = r;
    clear.float32[1] = g;
    clear.float32[2] = b;
    clear.float32[3] = 1.0f;
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cb, frames_[index].image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);

    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
}

void Application::present(uint32_t index, float r, float g, float b) {
    Framebuffer& fb = frames_[index];

    if (fb.counter > 0
        && !wait_release_point(fb.release_syncobj, fb.counter,
                               5LL * 1000LL * 1000LL * 1000LL)) {
        TRACE("deferring frame; release point " + std::to_string(fb.counter)
              + " not ready after resize or latch lag");
        return;
    }

    const uint64_t point = fb.counter + 1;
    TRACE("submit image=" + std::to_string(index) + " point=" + std::to_string(point));

    record_clear(index, r, g, b);

    VkExportFenceCreateInfo export_fence = {};
    export_fence.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
    export_fence.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.pNext = &export_fence;

    VkFence fence = VK_NULL_HANDLE;
    check(vkCreateFence(device_, &fence_info, nullptr, &fence), "vkCreateFence(sync_fd)");

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffers_[index];
    check(vkQueueSubmit(queue_, 1, &submit, fence), "vkQueueSubmit");

    materialize_acquire_point(fb.acquire_syncobj, point, fence);
    vkDestroyFence(device_, fence, nullptr);

    wp_linux_drm_syncobj_surface_v1_set_acquire_point(syncobj_surface_,
                                                      fb.wl_acquire_timeline, 0, point);
    wp_linux_drm_syncobj_surface_v1_set_release_point(syncobj_surface_,
                                                      fb.wl_release_timeline, 0, point);

    wl_surface_attach(surface_, fb.buffer, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface_);

    fb.counter = point;
    next_frame_ = (next_frame_ + 1) % BUFFER_COUNT;
}

void Application::run() {
    arm_frame();
    float rgb[3] = {};
    {
        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();
        hsv_to_rgb(std::fmod(seconds * 0.05f, 1.0f), 0.65f, 1.0f, rgb);
    }
    present(next_frame_, rgb[0], rgb[1], rgb[2]);

    const bool resize_test = std::getenv("CODOTAKU_RESIZE_TEST") != nullptr;
    int resize_test_step = 0;

    while (!closed_) {
        poll_and_dispatch();
        if (!frame_due_ && !resize_pending_) {
            continue;
        }
        if (resize_pending_) {
            handle_resize();
        }
        frame_due_ = false;
        if (frame_callback_ == nullptr) {
            arm_frame();
        }
        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();
        hsv_to_rgb(std::fmod(seconds * 0.05f, 1.0f), 0.65f, 1.0f, rgb);
        present(next_frame_, rgb[0], rgb[1], rgb[2]);
        if (!retired_.empty()) {
            reap_retired();
        }

        if (resize_test) {
            auto step_at = [&](int step) { return seconds >= static_cast<float>(step * 4); };
            if (resize_test_step == 0 && step_at(1)) {
                xdg_toplevel_set_fullscreen(toplevel_, nullptr);
                wl_display_flush(display_);
                ++resize_test_step;
            } else if (resize_test_step == 1 && step_at(3)) {
                xdg_toplevel_unset_fullscreen(toplevel_);
                wl_display_flush(display_);
                ++resize_test_step;
            } else if (resize_test_step == 2 && step_at(5)) {
                xdg_toplevel_set_fullscreen(toplevel_, nullptr);
                wl_display_flush(display_);
                ++resize_test_step;
            } else if (resize_test_step == 3 && step_at(7)) {
                xdg_toplevel_unset_fullscreen(toplevel_);
                wl_display_flush(display_);
                ++resize_test_step;
            }
        }
    }
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
}

} // namespace

int main() {
    try {
        Application app;
        app.run();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
