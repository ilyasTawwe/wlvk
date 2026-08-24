#include "platform_wayland.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>

namespace {

void check_display(wl_display* display) {
    if (const uint32_t error = wl_display_get_error(display); error != 0) {
        throw std::runtime_error("wayland display error: " + std::to_string(error));
    }
}

} // namespace

Window::Window() {
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        throw std::runtime_error("failed to connect to wayland display");
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener_, this);
    if (wl_display_roundtrip(display_) < 0) {
        throw std::runtime_error("wayland initial roundtrip failed");
    }
    check_display(display_);

    if (compositor_ == nullptr || wm_base_ == nullptr) {
        throw std::runtime_error("compositor does not expose wl_compositor and xdg_wm_base");
    }

    // Must answer pings or the compositor will consider us unresponsive.
    xdg_wm_base_add_listener(wm_base_, &wm_base_listener_, this);
}

Window::~Window() {
    if (frame_callback_ != nullptr) {
        wl_callback_destroy(frame_callback_);
    }
    if (toplevel_ != nullptr) {
        xdg_toplevel_destroy(toplevel_);
    }
    if (xdg_surface_ != nullptr) {
        xdg_surface_destroy(xdg_surface_);
    }
    if (surface_ != nullptr) {
        wl_surface_destroy(surface_);
    }
    if (wm_base_ != nullptr) {
        xdg_wm_base_destroy(wm_base_);
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
    }
    if (display_ != nullptr) {
        wl_display_disconnect(display_);
    }
}

void Window::registry_global(void* data, wl_registry* registry, uint32_t name,
                             const char* interface, uint32_t version) {
    auto* self = static_cast<Window*>(data);

    if (std::strcmp(interface, "wl_compositor") == 0 && self->compositor_ == nullptr) {
        const uint32_t ver = version < 4 ? version : 4;
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, ver));
    } else if (std::strcmp(interface, "xdg_wm_base") == 0 && self->wm_base_ == nullptr) {
        self->wm_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    }
}

void Window::registry_global_remove(void*, wl_registry*, uint32_t) {}

void Window::wm_base_ping(void*, xdg_wm_base* wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

void Window::create_toplevel(const char* title, const char* app_id) {
    surface_ = wl_compositor_create_surface(compositor_);
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);

    xdg_surface_add_listener(xdg_surface_, &xdg_surface_listener_, this);
    xdg_toplevel_add_listener(toplevel_, &toplevel_listener_, this);

    xdg_toplevel_set_title(toplevel_, title);
    xdg_toplevel_set_app_id(toplevel_, app_id);

    // Initial commit; the compositor responds with a configure event.
    wl_surface_commit(surface_);
}

void Window::xdg_surface_configure(void* data, xdg_surface* xdg_surface, uint32_t serial) {
    auto* self = static_cast<Window*>(data);
    xdg_surface_ack_configure(xdg_surface, serial);
    self->configured_ = true;
}

void Window::toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                                wl_array*) {
    auto* self = static_cast<Window*>(data);
    if (width > 0 && height > 0
        && (static_cast<uint32_t>(width) != self->width_
            || static_cast<uint32_t>(height) != self->height_)) {
        self->width_ = static_cast<uint32_t>(width);
        self->height_ = static_cast<uint32_t>(height);
        self->resized_ = true;
    }
}

void Window::toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}

void Window::toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

void Window::toplevel_close(void* data, xdg_toplevel*) {
    auto* self = static_cast<Window*>(data);
    self->closed_ = true;
}

void Window::wait_for_configure() {
    while (!configured_ && !closed_) {
        if (wl_display_dispatch(display_) < 0) {
            throw std::runtime_error("wayland dispatch failed while waiting for configure");
        }
        check_display(display_);
    }
}

void Window::arm_frame() {
    frame_callback_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_callback_, &frame_listener_, this);
    if (wl_display_flush(display_) < 0) {
        throw std::runtime_error("wayland flush failed");
    }
}

void Window::frame_done(void* data, wl_callback* callback, uint32_t) {
    auto* self = static_cast<Window*>(data);
    wl_callback_destroy(callback);
    self->frame_callback_ = nullptr;
    self->frame_due_ = true;
}

void Window::poll_and_dispatch() {
    if (!frame_due_ && !closed_) {
        pollfd pfd = {};
        pfd.fd = wl_display_get_fd(display_);
        pfd.events = POLLIN;
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            throw std::runtime_error("poll on wayland fd failed");
        }
    }
    // Drain already-queued events first...
    while (wl_display_dispatch_pending(display_) > 0) {
    }
    if (frame_due_ || closed_) {
        check_display(display_);
        return;
    }
    // ...then block until at least one event is read and dispatched.
    if (wl_display_dispatch(display_) < 0) {
        throw std::runtime_error("wayland dispatch failed");
    }
    while (wl_display_dispatch_pending(display_) > 0) {
    }
    check_display(display_);
}
