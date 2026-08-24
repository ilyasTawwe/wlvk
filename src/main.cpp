#include <wlvk/wlvk.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "xdg-shell-client-protocol.h"

namespace {

constexpr uint32_t kBufferCount = 3;

std::atomic<bool> g_shutdown{false};

void handle_signal(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

void install_signal_handlers() {
    struct sigaction sa = {};
    sa.sa_handler = &handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

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

void ck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed");
    }
}

void record_clear(VkCommandBuffer cb, VkImage image, float r, float g, float b) {
    ck(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ck(vkBeginCommandBuffer(cb, &begin), "vkBeginCommandBuffer");

    VkImageMemoryBarrier to_transfer = {};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
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
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);

    ck(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
}

}  // namespace

int main() {
    install_signal_handlers();

    wlvk::WindowConfig config;
    config.title = "codotaku-media";
    config.app_id = "codotaku-media";
    if (std::getenv("CODOTAKU_VALIDATION") != nullptr) {
        config.enable_validation = true;
    }
    if (std::getenv("CODOTAKU_TRACE") != nullptr) {
        config.log = [](std::string_view msg) {
            std::fprintf(stderr, "trace: %.*s\n", static_cast<int>(msg.size()), msg.data());
        };
    }
    if (std::getenv("CODOTAKU_PRESENT") != nullptr) {
        config.on_present = [](const wlvk::Stats& stats) {
            std::fprintf(stderr, "presented=%llu discarded=%llu late=%llu\n",
                         static_cast<unsigned long long>(stats.presented),
                         static_cast<unsigned long long>(stats.discarded),
                         static_cast<unsigned long long>(stats.late));
        };
    }

    try {
        wlvk::Window win(config);

        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(win.physical_device(), &props);
        std::printf("codotaku-media: %ux%u, gpu: %s\n", win.width(), win.height(),
                    props.deviceName);

        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = win.graphics_family();
        ck(vkCreateCommandPool(win.device(), &pool_info, nullptr, &command_pool),
           "vkCreateCommandPool");

        std::vector<VkCommandBuffer> command_buffers(kBufferCount);
        VkCommandBufferAllocateInfo cb_alloc = {};
        cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb_alloc.commandPool = command_pool;
        cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_alloc.commandBufferCount = kBufferCount;
        ck(vkAllocateCommandBuffers(win.device(), &cb_alloc, command_buffers.data()),
           "vkAllocateCommandBuffers");

        const auto start = std::chrono::steady_clock::now();
        const bool resize_test = std::getenv("CODOTAKU_RESIZE_TEST") != nullptr;
        int resize_test_step = 0;

        wlvk::Frame frame;
        while (!g_shutdown.load(std::memory_order_relaxed) && win.wait_frame(frame)) {
            const float seconds = std::chrono::duration<float>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
            float rgb[3];
            hsv_to_rgb(std::fmod(seconds * 0.05f, 1.0f), 0.65f, 1.0f, rgb);

            record_clear(command_buffers[frame.index], frame.image, rgb[0], rgb[1], rgb[2]);

            VkSubmitInfo submit = {};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_buffers[frame.index];
            ck(vkQueueSubmit(win.graphics_queue(), 1, &submit, frame.render_fence),
               "vkQueueSubmit");

            win.present(frame);

            if (resize_test) {
                auto step_at = [&](int step) { return seconds >= static_cast<float>(step * 4); };
                if (resize_test_step == 0 && step_at(1)) {
                    xdg_toplevel_set_fullscreen(win.toplevel(), nullptr);
                    wl_display_flush(win.display());
                    ++resize_test_step;
                } else if (resize_test_step == 1 && step_at(3)) {
                    xdg_toplevel_unset_fullscreen(win.toplevel());
                    wl_display_flush(win.display());
                    ++resize_test_step;
                } else if (resize_test_step == 2 && step_at(5)) {
                    xdg_toplevel_set_fullscreen(win.toplevel(), nullptr);
                    wl_display_flush(win.display());
                    ++resize_test_step;
                } else if (resize_test_step == 3 && step_at(7)) {
                    xdg_toplevel_unset_fullscreen(win.toplevel());
                    wl_display_flush(win.display());
                    ++resize_test_step;
                }
            }
        }

        ck(vkDeviceWaitIdle(win.device()), "vkDeviceWaitIdle");
        vkDestroyCommandPool(win.device(), command_pool, nullptr);

        const wlvk::Stats stats = win.present_stats();
        if (stats.presented != 0 || stats.discarded != 0) {
            std::fprintf(stderr,
                         "codotaku-media: presented=%llu discarded=%llu late=%llu"
                         " present-gap[min,max]=[%.2f,%.2f]ms\n",
                         static_cast<unsigned long long>(stats.presented),
                         static_cast<unsigned long long>(stats.discarded),
                         static_cast<unsigned long long>(stats.late),
                         stats.min_gap_ms, stats.max_gap_ms);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
