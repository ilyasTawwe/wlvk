#pragma once

#include <cstdint>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

class Window {
public:
    Window();
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void create_toplevel(const char* title, const char* app_id);

    void wait_for_configure();

    // Arms the frame callback; call only when no callback is outstanding.
    void arm_frame();

    bool frame_armed() const { return frame_callback_ != nullptr; }

    // Blocks until Wayland traffic arrives, then dispatches everything queued.
    void poll_and_dispatch();

    bool closed() const { return closed_; }

    // Returns true (and clears) once the compositor requests the next frame.
    bool take_frame_due() {
        const bool due = frame_due_;
        frame_due_ = false;
        return due;
    }

    wl_display* display() const { return display_; }
    wl_surface* surface() const { return surface_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool resized() const { return resized_; }
    void clear_resized() { resized_ = false; }

private:
    static void registry_global(void* data, wl_registry* registry, uint32_t name,
                                const char* interface, uint32_t version);
    static void registry_global_remove(void* data, wl_registry* registry, uint32_t name);
    static void wm_base_ping(void* data, xdg_wm_base* wm_base, uint32_t serial);
    static void xdg_surface_configure(void* data, xdg_surface* xdg_surface, uint32_t serial);
    static void toplevel_configure(void* data, xdg_toplevel* toplevel, int32_t width,
                                   int32_t height, wl_array* states);
    static void toplevel_configure_bounds(void* data, xdg_toplevel* toplevel, int32_t width,
                                          int32_t height);
    static void toplevel_wm_capabilities(void* data, xdg_toplevel* toplevel, wl_array* capabilities);
    static void toplevel_close(void* data, xdg_toplevel* toplevel);
    static void frame_done(void* data, wl_callback* callback, uint32_t time);

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

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    xdg_wm_base* wm_base_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    wl_callback* frame_callback_ = nullptr;

    bool configured_ = false;
    bool closed_ = false;
    bool frame_due_ = false;
    bool resized_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};
