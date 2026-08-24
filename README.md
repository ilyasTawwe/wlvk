# wlvk

A raylib-style windowing library for **Vulkan on Wayland** — you keep total
control of Vulkan, the library owns everything else: the Wayland connection,
the xdg-shell window, format/modifier negotiation, zero-copy dmabuf
presentation with explicit sync, resize handling, and presentation timing.

No swapchain. No `vkQueuePresentKHR`. Three calls:

```cpp
wlvk::Window win(config);
while (win.wait_frame(frame)) {
    // record commands against frame.image, then:
    vkQueueSubmit(queue, 1, &submit, frame.render_fence);
    win.present(frame);
}
```

## Why

The standard `VK_KHR_wayland_surface` path wraps your frames in a swapchain
abstraction that fights you the moment you want direct scanout, deterministic
frame pacing, or driver-independent present feedback — especially on NVIDIA,
where the swapchain path has a history of blocking and busy-spin issues.
`wlvk` skips it entirely:

| Stock WSI | wlvk |
|---|---|
| surface + capabilities + swapchain setup | one constructor |
| acquire/present semaphore dance | `wait_frame()` / `present()` |
| `OUT_OF_DATE` / `SUBOPTIMAL` rebuild state machine | automatic; `on_resize` callback |
| format/modifier negotiation across 3 query APIs | one optional callback |
| tearing risk or DIY explicit sync | built-in via `linux-drm-syncobj-v1` |
| no present feedback on desktop | `wp_presentation` stats included |

Images are allocated by you-the-library partnership with VMA as
modifier-decorated, dmabuf-exported `VkImage`s that the display controller can
scan out directly — the GPU writes the exact memory KMS displays. Zero copies.
Server-side decorations are requested by default (`prefer_server_decoration`)
and silently skipped when the compositor lacks xdg-decoration or overrides it.

## Requirements

- Linux, running under a Wayland compositor with:
  - `zwp_linux_dmabuf_v1` **version 4** (dmabuf feedback)
  - `wp_linux_drm_syncobj_manager_v1` (explicit sync)
  
  Both are hard requirements; the library refuses to start without them
  rather than silently falling back to a copy or tearing path.
- A Vulkan 1.4 driver exposing `VK_EXT_image_drm_format_modifier`,
  `VK_EXT_external_memory_dma_buf`, and the external semaphore/fence fd
  extensions (checked per-device during selection).
- Build: CMake ≥ 4.0, a C++26 compiler (GCC 14+ tested), plus system
  packages for `wayland-client`, `libdrm`, `wayland-protocols`,
  `wayland-scanner`, and [volk](https://github.com/zeux/volk) (headers + `volk.c`).

## Building

```sh
cmake --preset release && cmake --build --preset release
```

This produces `build/release/lib/wlvk/libwlvk.a` and the example binary
`build/release/examples/clear/wlvk-clear` (a rotating HSV clear that doubles
as a conformance smoke test). Debug preset works the same way.

Set `WLVK_BUILD_EXAMPLES=OFF` to build only the library.

## Usage

```cpp
#include <wlvk/wlvk.hpp>

int main() {
    wlvk::WindowConfig config;
    config.title   = "my-app";
    config.width   = 1280;
    config.height  = 720;
    config.log     = [](std::string_view msg) { std::fprintf(stderr, "%.*s\n",
                         int(msg.size()), msg.data()); };
    config.on_resize = [](uint32_t w, uint32_t h) { /* viewport update */ };
    // optional hooks: select_device, configure_device, choose_format, on_present

    wlvk::Window win(config);
    VkDevice device = win.device();          // add extensions/features via
                                             // config.configure_device(DeviceBuilder&)
    wlvk::Frame frame;
    while (win.wait_frame(frame)) {
        // frame.image: scanout VkImage (B8G8R8A8/R8G8B8A8 UNORM)
        // frame.render_fence: signal it with vkQueueSubmit when done
        // frame.pitch, frame.drm_modifier, frame.dma_buf_fd also available
        win.present(frame);                  // after submitting with render_fence
    }
}
```

Contracts worth knowing:

- `Frame` is valid until the next `wait_frame()` call or an `on_resize` fire;
  don't cache `image` handles across iterations.
- You submit; you signal `frame.render_fence`. `present()` extracts a sync fd
  from it, materializes the compositor's acquire point from it, and commits.
- `wait_frame()` returns `false` once the window is closed by the compositor.
- Per-frame pacing comes from `wl_surface.frame`; present timing/discards come
  from `wp_presentation` (`present_stats()`, `on_present`).

## How it works

Construction binds Wayland globals (compositor, xdg-shell, dmabuf v4,
syncobj manager, presentation-time), creates the Vulkan instance/device
(policy hooks let you pick devices and enable anything), then asks the
compositor what it can scan out via dmabuf feedback and intersects that with
driver modifier support for your chosen format.

Each frame cycle:

```
wait_frame   reap retired buffers → wait vsync frame-callback → handle
             pending resizes (rebuild images, fire on_resize) → block on the
             slot's release point (compositor finished reading it) → hand out
present      render_fence → sync fd → timeline point N as the compositor's
             acquire dependency; release point N frees the slot later
```

A fixed 3-slot ring keeps one frame in flight while two age on screen;
resize retires old generations and reaps them lazily once their release
points signal. See `lib/wlvk/` (~2000 lines) if you're curious — the whole
thing is deliberately small enough to read.

## Example

`examples/static/` is the hello-world: a static cornflower-blue clear using
nothing but library defaults — no configuration, no callbacks, no output.

`examples/clear/` renders a hue-cycling clear color through the full pipeline.
Environment knobs: `WLVK_VALIDATION=1` enables the validation layer,
`WLVK_TRACE=1` enables internal tracing, `WLVK_PRESENT_STATS=1` prints per-frame
presentation stats, `WLVK_RESIZE_TEST=1` toggles fullscreen periodically.

## License

MIT — see [LICENSE](LICENSE). Bundled dependencies in-tree:
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)
(vendored header). Links against system volk, libwayland, libdrm at build time.
