#ifndef WLVK_DETAIL_HPP_
#define WLVK_DETAIL_HPP_

#include "wlvk.hpp"

#include <wayland-client.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include <array>
#include <chrono>
#include <string>

#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

namespace wlvk::detail {

[[noreturn]] void fail(const std::string& what);
void check_vk(VkResult result, const char* what);

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
extern const char* const kRequiredDeviceExts[4];

#define WLVK_TRACE(impl, msg) do { if ((impl).config_.log != nullptr) (impl).log(msg); } while (false)

VkFormat vk_format_for_fourcc(uint32_t fourcc);

struct FrameSlot {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    int dma_buf_fd = -1;
    uint32_t pitch = 0;
    uint32_t plane_count = 1;
    std::array<PlaneInfo, 4> planes = {};
    wl_buffer* buffer = nullptr;
    uint32_t acquire_syncobj = 0;
    uint32_t release_syncobj = 0;
    wp_linux_drm_syncobj_timeline_v1* wl_acquire_timeline = nullptr;
    wp_linux_drm_syncobj_timeline_v1* wl_release_timeline = nullptr;
    int acquire_fd = -1;  // transient, only during setup
    int release_fd = -1;
    uint64_t counter = 0;
    bool released = false;  // retired-slot bookkeeping
    struct wp_presentation_feedback* present_feedback = nullptr;
    VkFence render_fence = VK_NULL_HANDLE;  // exportable, reused per slot
};

struct StatsAccum {
    uint64_t presented = 0;
    uint64_t discarded = 0;
    uint64_t late = 0;
    int64_t last_tv_ns = 0;
    int64_t min_gap_ns = 0;
    int64_t max_gap_ns = 0;
};

struct ModifierSupport {
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool blind = false;  // driver exposes no modifier list: accept anything
    std::vector<uint64_t> modifiers;
};

// Callbacks first: the constexpr listener tables below reference these by
// address, and static-data-member initializers do not get complete-class
// name lookup.
struct Callbacks {
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
    static void decoration_configure(void* data, zxdg_toplevel_decoration_v1*, uint32_t mode);
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

    static void presentation_sync_output(void*, struct wp_presentation_feedback*,
                                         wl_output*) {}
    static void presentation_presented(void* data, struct wp_presentation_feedback*,
                                       uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                       uint32_t, uint32_t);
    static void presentation_discarded(void* data, struct wp_presentation_feedback*);

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data);
};

struct Impl : Callbacks {
    explicit Impl(const WindowConfig& config);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void init_wayland();
    void check_display();
    void create_window();
    void poll_and_dispatch();
    void drain_events();
    void arm_frame();

    void init_vulkan();
    void create_instance();
    void pick_physical_device();
    void query_default_feedback();
    void resolve_format();
    void create_device();
    void create_allocator();
    void open_drm_node();
    void create_framebuffers();
    void create_dmabuf_resources();
    void handle_resize();
    void reap_retired();

    bool next_frame(Frame& out);
    void commit_frame(Frame& frame);

    uint32_t create_syncobj();
    wp_linux_drm_syncobj_timeline_v1* share_syncobj(uint32_t syncobj, int* out_fd);
    void materialize_acquire_point(uint32_t syncobj, uint64_t point, VkFence fence);
    bool wait_release_point(uint32_t syncobj, uint64_t point, int64_t timeout_ns);

    ModifierSupport& modifier_support(VkFormat format);

    void log(std::string msg) const {
        if (config_.log != nullptr) {
            config_.log(msg);
        }
    }

    WindowConfig config_;

    // wayland
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
    wp_presentation* presentation_ = nullptr;
    zxdg_decoration_manager_v1* decoration_manager_ = nullptr;
    zxdg_toplevel_decoration_v1* decoration_ = nullptr;
    uint32_t decoration_mode_ = ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;

    // dmabuf feedback state
    std::vector<uint8_t> format_table_;
    std::vector<std::pair<uint32_t, uint64_t>> tranche_candidates_;
    std::vector<FormatOffer> offers_;
    std::vector<FormatOffer> driver_ok_;
    uint32_t chosen_fourcc_ = DRM_FORMAT_XRGB8888;
    VkFormat chosen_vk_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    uint64_t chosen_modifier_ = 0;
    std::vector<ModifierSupport> modifier_cache_;
    dev_t main_device_ = 0;

    StatsAccum stats_;

    int drm_fd_ = -1;

    bool configured_ = false;
    bool closed_ = false;
    bool frame_due_ = false;
    bool resize_pending_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t pending_width_ = 0;
    uint32_t pending_height_ = 0;

    // vulkan
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    uint32_t graphics_family_ = 0;
    uint32_t present_family_ = 0;
    std::string gpu_name_;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VmaPool dmabuf_pool_ = nullptr;
    VkExportMemoryAllocateInfo dmabuf_export_{};
    VkQueue queue_ = VK_NULL_HANDLE;
    bool budget_ext_ = false;  // VK_EXT_memory_budget advertised by the device

    std::vector<FrameSlot> frames_;
    std::vector<FrameSlot> retired_;
    uint32_t next_frame_ = 0;
    uint32_t handed_index_ = UINT32_MAX;
    bool presented_once_ = false;  // first wait_frame must not wait for a frame
                                   // callback: nothing has been committed yet

    static constexpr wl_registry_listener registry_listener_ = {
        .global = &Callbacks::registry_global,
        .global_remove = &Callbacks::registry_global_remove,
    };
    static constexpr xdg_wm_base_listener wm_base_listener_ = {
        .ping = &Callbacks::wm_base_ping,
    };
    static constexpr xdg_surface_listener xdg_surface_listener_ = {
        .configure = &Callbacks::xdg_surface_configure,
    };
    static constexpr xdg_toplevel_listener toplevel_listener_ = {
        .configure = &Callbacks::toplevel_configure,
        .close = &Callbacks::toplevel_close,
        .configure_bounds = &Callbacks::toplevel_configure_bounds,
        .wm_capabilities = &Callbacks::toplevel_wm_capabilities,
    };
    static constexpr wl_callback_listener frame_listener_ = {
        .done = &Callbacks::frame_done,
    };
    static constexpr zwp_linux_dmabuf_v1_listener dmabuf_listener_ = {
        .format = &Callbacks::dmabuf_format,
        .modifier = &Callbacks::dmabuf_modifier,
    };
    static constexpr zwp_linux_dmabuf_feedback_v1_listener feedback_listener_ = {
        .done = &Callbacks::feedback_done,
        .format_table = &Callbacks::feedback_format_table,
        .main_device = &Callbacks::feedback_main_device,
        .tranche_done = &Callbacks::feedback_tranche_done,
        .tranche_target_device = &Callbacks::feedback_tranche_target_device,
        .tranche_formats = &Callbacks::feedback_tranche_formats,
        .tranche_flags = &Callbacks::feedback_tranche_flags,
    };
    static constexpr struct wp_presentation_feedback_listener presentation_listener_ = {
        .sync_output = &Callbacks::presentation_sync_output,
        .presented = &Callbacks::presentation_presented,
        .discarded = &Callbacks::presentation_discarded,
    };
    static constexpr zxdg_toplevel_decoration_v1_listener decoration_listener_ = {
        .configure = &Callbacks::decoration_configure,
    };
};

}  // namespace wlvk::detail

#endif  // WLVK_DETAIL_HPP_
