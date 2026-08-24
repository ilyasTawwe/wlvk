#include "detail.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <poll.h>

#include <unistd.h>

namespace wlvk::detail {

// ---- Wayland listeners ----

void Callbacks::registry_global(void* data, wl_registry* registry, uint32_t name,
                                const char* interface, uint32_t version) {
    auto* self = static_cast<Impl*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0 && self->compositor_ == nullptr) {
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             version < 4 ? version : 4));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0 && self->wm_base_ == nullptr) {
        self->wm_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(self->wm_base_, &Impl::wm_base_listener_, self);
    } else if (std::strcmp(interface, "zwp_linux_dmabuf_v1") == 0 && self->dmabuf_ == nullptr) {
        self->dmabuf_ = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
                             version < 4 ? version : 4));
        zwp_linux_dmabuf_v1_add_listener(self->dmabuf_, &Impl::dmabuf_listener_, self);
    } else if (std::strcmp(interface, "wp_linux_drm_syncobj_manager_v1") == 0
               && self->syncobj_manager_ == nullptr) {
        self->syncobj_manager_ = static_cast<wp_linux_drm_syncobj_manager_v1*>(
            wl_registry_bind(registry, name, &wp_linux_drm_syncobj_manager_v1_interface, 1));
    } else if (std::strcmp(interface, "wp_presentation") == 0
               && self->presentation_ == nullptr) {
        self->presentation_ = static_cast<wp_presentation*>(
            wl_registry_bind(registry, name, &wp_presentation_interface,
                             version < 1 ? version : 1));
    }
}

void Callbacks::wm_base_ping(void*, xdg_wm_base* wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

void Callbacks::xdg_surface_configure(void* data, xdg_surface* xdg_surface, uint32_t serial) {
    auto* self = static_cast<Impl*>(data);
    xdg_surface_ack_configure(xdg_surface, serial);
    self->configured_ = true;
}

void Callbacks::toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                                   wl_array*) {
    auto* self = static_cast<Impl*>(data);
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

void Callbacks::toplevel_close(void* data, xdg_toplevel*) {
    static_cast<Impl*>(data)->closed_ = true;
}

void Callbacks::frame_done(void* data, wl_callback* callback, uint32_t) {
    auto* self = static_cast<Impl*>(data);
    wl_callback_destroy(callback);
    self->frame_callback_ = nullptr;
    self->frame_due_ = true;
}

// ---- lifecycle ----

Impl::~Impl() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
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
    if (syncobj_surface_ != nullptr) {
        wp_linux_drm_syncobj_surface_v1_destroy(syncobj_surface_);
    }
    for (auto& frame : frames_) {
        if (frame.present_feedback != nullptr) {
            wp_presentation_feedback_destroy(frame.present_feedback);
            frame.present_feedback = nullptr;
        }
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
        if (frame.render_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frame.render_fence, nullptr);
        }
        if (frame.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, frame.image, frame.allocation);
        }
    }
    if (dmabuf_pool_ != nullptr) {
        vmaDestroyPool(allocator_, dmabuf_pool_);
    }
    vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    if (debug_messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    }
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

void Impl::check_display() {
    if (const uint32_t error = wl_display_get_error(display_); error != 0) {
        fail("wayland display error: " + std::to_string(error));
    }
}

void Impl::init_wayland() {
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        fail("failed to connect to wayland display");
    }
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener_, this);
    if (wl_display_roundtrip(display_) < 0) {
        fail("wayland roundtrip failed");
    }
    check_display();
    if (compositor_ == nullptr || wm_base_ == nullptr) {
        fail("missing wl_compositor / xdg_wm_base");
    }
    if (dmabuf_ == nullptr || syncobj_manager_ == nullptr) {
        fail("compositor lacks linux-dmabuf / linux-drm-syncobj; explicit sync unavailable");
    }
    create_window();
    while (!configured_ && !closed_) {
        if (wl_display_dispatch(display_) < 0) {
            fail("wayland dispatch failed");
        }
        check_display();
    }
    if (!configured_) {
        fail("compositor closed window before configure");
    }
    width_ = pending_width_ ? pending_width_ : config_.width;
    height_ = pending_height_ ? pending_height_ : config_.height;
}

void Impl::create_window() {
    surface_ = wl_compositor_create_surface(compositor_);
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_surface_add_listener(xdg_surface_, &xdg_surface_listener_, this);
    xdg_toplevel_add_listener(toplevel_, &toplevel_listener_, this);
    xdg_toplevel_set_title(toplevel_, config_.title);
    xdg_toplevel_set_app_id(toplevel_, config_.app_id);
    wl_surface_commit(surface_);
}

void Impl::poll_and_dispatch() {
    if (!frame_due_ && !closed_) {
        if (wl_display_flush(display_) < 0) {
            fail("wayland flush failed");
        }
        pollfd pfd = {};
        pfd.fd = wl_display_get_fd(display_);
        pfd.events = POLLIN;
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            fail("poll on wayland fd failed");
        }
    }
    while (wl_display_dispatch_pending(display_) > 0) {
    }
    if (frame_due_ || closed_) {
        return;
    }
    if (wl_display_dispatch(display_) < 0) {
        fail("wayland dispatch failed");
    }
    while (wl_display_dispatch_pending(display_) > 0) {
    }
    check_display();
}

void Impl::drain_events() {
    if (wl_display_flush(display_) < 0) {
        fail("wayland flush failed");
    }
    pollfd pfd = {};
    pfd.fd = wl_display_get_fd(display_);
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
        if (wl_display_dispatch(display_) < 0) {
            fail("wayland dispatch failed");
        }
    }
    while (wl_display_dispatch_pending(display_) > 0) {
    }
    check_display();
}

void Impl::arm_frame() {
    frame_callback_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_callback_, &frame_listener_, this);
    if (wl_display_flush(display_) < 0) {
        fail("wayland flush failed");
    }
}

void Impl::init_vulkan() {
    check_vk(volkInitialize(), "volkInitialize");
    create_instance();
    pick_physical_device();
    query_default_feedback();
    create_device();
    create_allocator();
    open_drm_node();
    create_framebuffers();
}

// ---- buffers ----

void Impl::create_framebuffers() {
    frames_.resize(config_.buffer_count);

    dmabuf_export_.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    dmabuf_export_.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    const bool linear = chosen_modifier_ == 0;

    VkImageCreateInfo pool_image_info = {};
    pool_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    pool_image_info.imageType = VK_IMAGE_TYPE_2D;
    pool_image_info.format = chosen_vk_format_;
    pool_image_info.extent = {width_, height_, 1};
    pool_image_info.mipLevels = 1;
    pool_image_info.arrayLayers = 1;
    pool_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    pool_image_info.tiling = linear ? VK_IMAGE_TILING_LINEAR
                                    : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    pool_image_info.usage = config_.image_usage;
    pool_image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    pool_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo pool_alloc_template = {};
    pool_alloc_template.usage = VMA_MEMORY_USAGE_AUTO;
    uint32_t mem_type_index = 0;
    if (linear) {
        check_vk(vmaFindMemoryTypeIndexForImageInfo(allocator_, &pool_image_info,
                                                    &pool_alloc_template, &mem_type_index),
                 "vmaFindMemoryTypeIndexForImageInfo");
    } else {
        VkImageCreateInfo query_info = pool_image_info;
        query_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        query_info.pNext = nullptr;
        check_vk(vmaFindMemoryTypeIndexForImageInfo(allocator_, &query_info,
                                                    &pool_alloc_template, &mem_type_index),
                 "vmaFindMemoryTypeIndexForImageInfo");
    }

    VmaPoolCreateInfo pool_info = {};
    pool_info.memoryTypeIndex = mem_type_index;
    pool_info.pMemoryAllocateNext = &dmabuf_export_;
    check_vk(vmaCreatePool(allocator_, &pool_info, &dmabuf_pool_), "vmaCreatePool(dmabuf)");

    create_dmabuf_resources();

    for (uint32_t i = 0; i < frames_.size(); ++i) {
        FrameSlot& fb = frames_[i];

        fb.acquire_syncobj = create_syncobj();
        fb.wl_acquire_timeline = share_syncobj(fb.acquire_syncobj, &fb.acquire_fd);
        if (fb.wl_acquire_timeline == nullptr) {
            fail("import_timeline(acquire) failed");
        }

        fb.release_syncobj = create_syncobj();
        fb.wl_release_timeline = share_syncobj(fb.release_syncobj, &fb.release_fd);
        if (fb.wl_release_timeline == nullptr) {
            fail("import_timeline(release) failed");
        }

        VkExportFenceCreateInfo export_fence = {};
        export_fence.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
        export_fence.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.pNext = &export_fence;
        check_vk(vkCreateFence(device_, &fence_info, nullptr, &fb.render_fence),
                 "vkCreateFence(sync_fd)");
    }

    if (wl_display_roundtrip(display_) < 0) {
        fail("wayland roundtrip after timeline import failed");
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
        fail("get_surface(syncobj) failed");
    }
}

void Impl::create_dmabuf_resources() {
    const bool linear = chosen_modifier_ == 0;

    VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {};
    if (!linear) {
        modifier_list.sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
        modifier_list.drmFormatModifierCount = 1;
        modifier_list.pDrmFormatModifiers = &chosen_modifier_;
    }

    for (uint32_t i = 0; i < frames_.size(); ++i) {
        FrameSlot& fb = frames_[i];

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
        image_info.format = chosen_vk_format_;
        image_info.extent = {width_, height_, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = linear ? VK_IMAGE_TILING_LINEAR
                                   : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        image_info.usage = config_.image_usage;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        alloc_info.pool = dmabuf_pool_;

        check_vk(vmaCreateImage(allocator_, &image_info, &alloc_info, &fb.image,
                                &fb.allocation, nullptr),
                 "vmaCreateImage(dmabuf)");

        VmaAllocationInfo alloc_result = {};
        vmaGetAllocationInfo(allocator_, fb.allocation, &alloc_result);

        VkMemoryGetFdInfoKHR get_fd = {};
        get_fd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        get_fd.memory = alloc_result.deviceMemory;
        get_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        check_vk(vkGetMemoryFdKHR(device_, &get_fd, &fb.dma_buf_fd), "vkGetMemoryFdKHR");

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
                                                            chosen_fourcc_, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (fb.buffer == nullptr) {
            fail("zwp_linux_buffer_params_v1_create_immed failed");
        }
        WLVK_TRACE(*this, "image " + std::to_string(i) + ": fd="
                              + std::to_string(fb.dma_buf_fd) + " pitch="
                              + std::to_string(fb.pitch) + " " + std::to_string(width_) + "x"
                              + std::to_string(height_));
    }
}

// ---- resize ----

void Impl::handle_resize() {
    WLVK_TRACE(*this, "resize: " + std::to_string(width_) + "x" + std::to_string(height_)
                          + " -> " + std::to_string(pending_width_) + "x"
                          + std::to_string(pending_height_));
    check_vk(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(resize)");

    std::vector<FrameSlot> fresh(config_.buffer_count);
    for (uint32_t i = 0; i < config_.buffer_count; ++i) {
        FrameSlot& old = frames_[i];
        FrameSlot& dst = fresh[i];

        dst.acquire_syncobj = old.acquire_syncobj;
        dst.release_syncobj = old.release_syncobj;
        dst.acquire_fd = old.acquire_fd;
        dst.release_fd = old.release_fd;
        dst.wl_acquire_timeline = old.wl_acquire_timeline;
        dst.wl_release_timeline = old.wl_release_timeline;
        dst.counter = old.counter;
        dst.render_fence = old.render_fence;

        FrameSlot retired_fb;
        retired_fb.image = old.image;
        retired_fb.allocation = old.allocation;
        retired_fb.dma_buf_fd = old.dma_buf_fd;
        retired_fb.pitch = old.pitch;
        retired_fb.buffer = old.buffer;
        retired_fb.release_syncobj = old.release_syncobj;
        retired_fb.counter = old.counter;
        retired_.push_back(retired_fb);

        if (old.present_feedback != nullptr) {
            wp_presentation_feedback_destroy(old.present_feedback);
            old.present_feedback = nullptr;
        }
        old.image = VK_NULL_HANDLE;
        old.allocation = nullptr;
        old.dma_buf_fd = -1;
        old.pitch = 0;
        old.buffer = nullptr;
        old.render_fence = VK_NULL_HANDLE;
    }
    width_ = pending_width_;
    height_ = pending_height_;
    resize_pending_ = false;

    frames_ = std::move(fresh);
    handed_index_ = UINT32_MAX;
    create_dmabuf_resources();

    if (config_.on_resize != nullptr) {
        config_.on_resize(width_, height_);
    }
}

void Impl::reap_retired() {
    constexpr int64_t wait_ns = 100LL * 1000LL * 1000LL;
    for (auto it = retired_.begin(); it != retired_.end();) {
        FrameSlot& fb = *it;
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

// ---- frame flow ----

bool Impl::next_frame(Frame& out) {
    if (!retired_.empty()) {
        reap_retired();
    }

    // A wl_frame callback only fires once a commit carries it; before the
    // first present() there is nothing to wait for, so hand out a slot
    // immediately.
    if (presented_once_) {
        while (!frame_due_ && !closed_) {
            poll_and_dispatch();
        }
        if (closed_) {
            return false;
        }
        if (resize_pending_) {
            handle_resize();
        }
    } else if (resize_pending_) {
        handle_resize();
    }
    frame_due_ = false;
    if (frame_callback_ == nullptr && !closed_) {
        arm_frame();
    }

    FrameSlot& fb = frames_[next_frame_];
    while (fb.counter > 0
           && !wait_release_point(fb.release_syncobj, fb.counter, 100LL * 1000LL * 1000LL)) {
        drain_events();
        if (closed_) {
            return false;
        }
    }

    handed_index_ = next_frame_;
    out.index = next_frame_;
    out.format = chosen_vk_format_;
    out.image = fb.image;
    out.pitch = fb.pitch;
    out.drm_modifier = chosen_modifier_;
    out.dma_buf_fd = fb.dma_buf_fd;
    out.render_fence = fb.render_fence;
    return true;
}

void Impl::commit_frame(Frame& frame) {
    if (frame.index != handed_index_ || handed_index_ >= frames_.size()) {
        fail("present called with a stale or unknown Frame");
    }
    FrameSlot& fb = frames_[frame.index];

    const uint64_t point = fb.counter + 1;
    WLVK_TRACE(*this, "submit image=" + std::to_string(frame.index)
                          + " point=" + std::to_string(point));

    materialize_acquire_point(fb.acquire_syncobj, point, fb.render_fence);
    check_vk(vkResetFences(device_, 1, &fb.render_fence), "vkResetFences");

    wp_linux_drm_syncobj_surface_v1_set_acquire_point(syncobj_surface_,
                                                      fb.wl_acquire_timeline, 0, point);
    wp_linux_drm_syncobj_surface_v1_set_release_point(syncobj_surface_,
                                                      fb.wl_release_timeline, 0, point);

    if (presentation_ != nullptr && config_.collect_present_stats) {
        fb.present_feedback = wp_presentation_feedback(presentation_, surface_);
        if (fb.present_feedback != nullptr) {
            wp_presentation_feedback_add_listener(fb.present_feedback,
                                                  &presentation_listener_, this);
        }
    }

    wl_surface_attach(surface_, fb.buffer, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface_);

    fb.counter = point;
    handed_index_ = UINT32_MAX;
    presented_once_ = true;
    next_frame_ = (next_frame_ + 1) % static_cast<uint32_t>(frames_.size());
}

// ---- presentation feedback ----

Stats Impl_stats(const StatsAccum& s) {
    Stats out;
    out.presented = s.presented;
    out.discarded = s.discarded;
    out.late = s.late;
    out.min_gap_ms = s.min_gap_ns / 1e6;
    out.max_gap_ms = s.max_gap_ns / 1e6;
    return out;
}

void Callbacks::presentation_presented(void* data, struct wp_presentation_feedback* pfb,
                                       uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                                       uint32_t tv_nsec, uint32_t refresh, uint32_t,
                                       uint32_t, uint32_t) {
    auto* self = static_cast<Impl*>(data);
    StatsAccum& s = self->stats_;
    const int64_t tv_ns =
        ((static_cast<int64_t>(tv_sec_hi) << 32) | static_cast<int64_t>(tv_sec_lo))
            * 1000000000LL
        + tv_nsec;
    if (s.last_tv_ns != 0) {
        const int64_t gap = tv_ns - s.last_tv_ns;
        if (s.min_gap_ns == 0 || gap < s.min_gap_ns) {
            s.min_gap_ns = gap;
        }
        if (gap > s.max_gap_ns) {
            s.max_gap_ns = gap;
        }
        if (refresh > 0 && gap > static_cast<int64_t>(refresh) * 3 / 2) {
            ++s.late;
        }
    }
    s.last_tv_ns = tv_ns;
    ++s.presented;

    for (auto& f : self->frames_) {
        if (f.present_feedback == pfb) {
            f.present_feedback = nullptr;
            break;
        }
    }
    wp_presentation_feedback_destroy(pfb);

    if (self->config_.on_present != nullptr) {
        self->config_.on_present(Impl_stats(self->stats_));
    }
}

void Callbacks::presentation_discarded(void* data, struct wp_presentation_feedback* pfb) {
    auto* self = static_cast<Impl*>(data);
    ++self->stats_.discarded;
    for (auto& f : self->frames_) {
        if (f.present_feedback == pfb) {
            f.present_feedback = nullptr;
            break;
        }
    }
    wp_presentation_feedback_destroy(pfb);

    if (self->config_.on_present != nullptr) {
        self->config_.on_present(Impl_stats(self->stats_));
    }
}

// ---- public Window forwarding ----

}  // namespace wlvk::detail

namespace wlvk {

Window::Window(const WindowConfig& config) : impl_(new detail::Impl(config)) {}
Window::~Window() = default;

bool Window::wait_frame(Frame& frame) { return impl_->next_frame(frame); }
void Window::present(Frame& frame) { impl_->commit_frame(frame); }

VkInstance Window::instance() const { return impl_->instance_; }
VkPhysicalDevice Window::physical_device() const { return impl_->physical_device_; }
VkDevice Window::device() const { return impl_->device_; }
VmaAllocator Window::allocator() const { return impl_->allocator_; }
VkQueue Window::graphics_queue() const { return impl_->queue_; }
uint32_t Window::graphics_family() const { return impl_->graphics_family_; }
uint32_t Window::present_family() const { return impl_->present_family_; }
int Window::drm_fd() const { return impl_->drm_fd_; }
wl_display* Window::display() { return impl_->display_; }
wl_surface* Window::surface() { return impl_->surface_; }
xdg_toplevel* Window::toplevel() { return impl_->toplevel_; }

uint32_t Window::width() const { return impl_->width_; }
uint32_t Window::height() const { return impl_->height_; }
std::string_view Window::gpu_name() const { return impl_->gpu_name_; }

Stats Window::present_stats() const { return detail::Impl_stats(impl_->stats_); }

}  // namespace wlvk
