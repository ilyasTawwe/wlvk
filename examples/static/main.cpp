#include <wlvk/wlvk.hpp>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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
    try {
        wlvk::Window win{wlvk::WindowConfig{}};

        constexpr uint32_t kBufferCount = 3;

        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = win.graphics_family();
        VkCommandPool command_pool = VK_NULL_HANDLE;
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

        constexpr float kColor[3] = {0.392f, 0.584f, 0.929f};

        wlvk::Frame frame;
        while (win.wait_frame(frame)) {
            record_clear(command_buffers[frame.index], frame.image, kColor[0],
                         kColor[1], kColor[2]);

            VkSubmitInfo submit = {};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_buffers[frame.index];
            ck(vkQueueSubmit(win.graphics_queue(), 1, &submit, frame.render_fence),
               "vkQueueSubmit");

            win.present(frame);
        }

        ck(vkDeviceWaitIdle(win.device()), "vkDeviceWaitIdle");
        vkDestroyCommandPool(win.device(), command_pool, nullptr);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
