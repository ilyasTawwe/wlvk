#include <cstdio>
#include <exception>

#include "platform_wayland.hpp"
#include "renderer_vulkan.hpp"

int main() {
    try {
        Window window;
        window.create_toplevel("codotaku media", "codotaku-media");
        window.wait_for_configure();
        std::printf("configured: %ux%u\n", window.width(), window.height());
        std::fflush(stdout);

        Renderer renderer(window.display(), window.surface(),
                          window.width() ? window.width() : 1280,
                          window.height() ? window.height() : 720);
        std::fflush(stdout);

        // Arm BEFORE drawing: a wl_surface.frame callback attaches to the next
        // commit, and vkQueuePresentKHR performs that commit synchronously.
        // Arming after a present leaves the callback stranded with no commit
        // to carry it, so it would never fire.
        window.arm_frame();
        renderer.draw_frame();
        window.clear_resized(); // the initial configure was already accounted for

        while (!window.closed()) {
            window.poll_and_dispatch();

            bool need_draw = window.take_frame_due();

            if (window.resized()) {
                renderer.notify_resized(window.width(), window.height());
                window.clear_resized();
                need_draw = true;
            }

            if (need_draw) {
                if (!window.frame_armed()) {
                    window.arm_frame(); // must precede this frame's commit
                }
                renderer.draw_frame();
            }
        }
        renderer.wait_idle();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
