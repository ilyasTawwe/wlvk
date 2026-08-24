// wlvk - minimal Wayland + Vulkan windowing library.
//
// Owns all window-system plumbing: xdg-shell window, linux-dmabuf-v4
// modifier negotiation, explicit sync (linux-drm-syncobj-v1), dmabuf-backed
// frame images, presentation timing, and the resize/reap lifecycle.
//
// You own everything else: record any commands you like, submit on any
// queue, enable extra instance/device extensions and features, and pick
// policy (device, format/modifier) via callbacks. Raw Vulkan and Wayland
// handles are always available.
//
// Presentation path is manual dmabuf with explicit sync only; compositors
// without zwp_linux_dmabuf_v1 v4 + wp_linux_drm_syncobj_manager_v1 are
// rejected at construction.
#ifndef WLVK_HPP_
#define WLVK_HPP_

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct wl_display;
struct wl_surface;
struct xdg_toplevel;

namespace wlvk {

namespace detail {
struct Impl;
}

/// One (format, modifier) pair offered by the compositor's dmabuf feedback.
struct FormatOffer {
    uint32_t drm_fourcc = 0;   // DRM_FORMAT_* constant
    VkFormat vk_format = VK_FORMAT_UNDEFINED;  // Vulkan equivalent; UNDEFINED if unmapped
    uint64_t modifier = 0;     // DRM format modifier (0 == LINEAR)
};

/// A physical device that satisfies the library's minimum requirements:
/// a graphics family, Wayland present support, and all device extensions
/// the library needs (external memory/fence/semaphore fd + drm modifiers).
struct DeviceCandidate {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    const VkPhysicalDeviceProperties* properties = nullptr;
    std::span<const VkExtensionProperties> extensions;
    std::span<const VkQueueFamilyProperties> queue_families;
};

/// Passed to WindowConfig::configure_device just before vkCreateDevice.
/// Lets you append device extensions and hook feature pNext chains.
class DeviceBuilder {
public:
    VkPhysicalDevice physical_device() const { return physical_device_; }

    /// Adds extensions to be enabled; duplicates are ignored.
    void add_extensions(std::span<const char* const> extensions);

    /// Appends a pNext struct (e.g. VkPhysicalDeviceVulkan14Features) to the
    /// VkDeviceCreateInfo chain. The struct must stay alive until Window()
    /// returns. Repeated calls append to the tail.
    void chain_next(void* structure);

private:
    friend struct detail::Impl;
    DeviceBuilder(VkPhysicalDevice pd, std::vector<const char*>* extensions, void** pnext_head)
        : physical_device_(pd), extensions_(extensions), pnext_head_(pnext_head) {}

    VkPhysicalDevice physical_device_;
    std::vector<const char*>* extensions_;
    void** pnext_head_;
};

struct WindowConfig {
    const char* title = "wlvk";
    const char* app_id = "wlvk";
    uint32_t width = 1280;      // used if compositor doesn't suggest a size
    uint32_t height = 720;
    uint32_t buffer_count = 3;

    uint32_t api_version = VK_API_VERSION_1_4;
    bool enable_validation = false;  // VK_LAYER_KHRONOS_validation + debug utils

    /// Request compositor-provided decorations (titlebar/borders) for the
    /// toplevel via xdg-decoration. Ignored when the compositor does not
    /// advertise the protocol, or when it overrides the mode (e.g. tiling).
    bool prefer_server_decoration = true;

    /// Usage flags for the per-frame dmabuf images. Capability filtering of
    /// modifiers accounts for these. TRANSFER_DST-only keeps the default
    /// modifier choice maximally permissive; adding usages may narrow it.
    VkImageUsageFlags image_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    std::vector<const char*> extra_instance_extensions;
    std::vector<const char*> extra_instance_layers;

    /// Optional hook before vkCreateDevice: add extensions/features beyond
    /// the library-required set. May query physical_device() freely.
    std::function<void(DeviceBuilder&)> configure_device;

    /// Optional policy: choose among candidates that already satisfy the
    /// library minimums. Return the index, or npos/>= size to abort.
    /// Default: first candidate.
    std::function<uint32_t(std::span<const DeviceCandidate>)> select_device;

    /// Optional policy: pick a format+modifier. `offers` is in compositor
    /// priority order; `driver_ok` is the subset whose modifier the driver
    /// supports for its format with the configured image usage. Return
    /// nullopt or an offer not present in `offers` to fail construction.
    /// Default: first entry of driver_ok.
    std::function<std::optional<FormatOffer>(std::span<const FormatOffer> offers,
                                             std::span<const FormatOffer> driver_ok)>
        choose_format;

    /// Called after buffers were recreated for a new size, from within
    /// wait_frame(). Frames handed out earlier are dead: recreate anything
    /// derived from image size here.
    std::function<void(uint32_t width, uint32_t height)> on_resize;

    /// Called from event dispatch after each presented/discarded feedback.
    std::function<void(const struct Stats&)> on_present;

    /// Diagnostic sink (replaces trace env vars). Called from anywhere on
    /// the main thread.
    std::function<void(std::string_view)> log;

    bool collect_present_stats = true;  // attach wp_presentation_feedback objects
};

struct Stats {
    uint64_t presented = 0;
    uint64_t discarded = 0;
    uint64_t late = 0;
    double min_gap_ms = 0.0;
    double max_gap_ms = 0.0;
};

/// A writable frame handed out by wait_frame(). Valid until the next
/// wait_frame() call (or until on_resize fires). All handles are
/// library-owned unless noted.
struct Frame {
    uint32_t index = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;   // write this
    uint32_t pitch = 0;               // bytes per row
    uint64_t drm_modifier = 0;
    int dma_buf_fd = -1;              // borrowed; never close
    /// Exportable (VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT) fence.
    /// Pass it as the signal fence of the vkQueueSubmit(s) that render this
    /// frame, then call present(). Do not reset, destroy, wait on it, or
    /// reuse it otherwise.
    VkFence render_fence = VK_NULL_HANDLE;
};

class Window {
public:
    /// Connects, negotiates formats, creates instance -> device -> buffers.
    /// Throws std::runtime_error on any failure.
    explicit Window(const WindowConfig& config = {});
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// Pumps events until the next vsync-aligned frame slot is free.
    /// Handles resize internally (invoking on_resize) and reaps retired
    /// buffers. Blocks until the slot's previous scanout has been released,
    /// so the returned image is safe to render into immediately.
    /// Returns false once the window was closed.
    bool wait_frame(Frame& frame);

    /// Commits the frame to the surface. Contract: between wait_frame() and
    /// present() you submitted work signalling frame.render_fence. present()
    /// never blocks. Only valid with the Frame most recently returned by
    /// wait_frame(); calling twice with the same Frame throws.
    void present(Frame& frame);

    // ---- raw handles: full control lives here ----
    VkInstance instance() const;
    VkPhysicalDevice physical_device() const;
    VkDevice device() const;
    VmaAllocator allocator() const;
    VkQueue graphics_queue() const;
    uint32_t graphics_family() const;
    uint32_t present_family() const;
    int drm_fd() const;           // DRM render node used for syncobjs
    wl_display* display();
    wl_surface* surface();
    xdg_toplevel* toplevel();

    uint32_t width() const;
    uint32_t height() const;
    std::string_view gpu_name() const;

    Stats present_stats() const;

private:
    std::unique_ptr<detail::Impl> impl_;
};

}  // namespace wlvk

#endif  // WLVK_HPP_
