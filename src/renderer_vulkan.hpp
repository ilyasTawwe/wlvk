#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <wayland-client.h>
#include <volk.h>
#include <vk_mem_alloc.h>

struct QueueFamilies {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;

    bool complete() const { return graphics.has_value() && present.has_value(); }
};

class Renderer {
public:
    Renderer(wl_display* display, wl_surface* surface, uint32_t width, uint32_t height);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void draw_frame();
    void notify_resized(uint32_t width, uint32_t height);
    void wait_idle() const;

private:
    void create_instance();
    void create_surface();
    void pick_physical_device();
    void create_device();
    void create_allocator();
    void create_swapchain(VkSwapchainKHR old_swapchain);
    void create_image_views();
    void create_command_buffers();
    void allocate_command_buffers();
    void create_sync_objects();
    void recreate_swapchain();
    void record_command_buffer(uint32_t image_index);

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data);

    wl_display* display_ = nullptr;
    wl_surface* wayland_surface_ = nullptr;
    uint32_t requested_width_ = 0;
    uint32_t requested_height_ = 0;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    QueueFamilies queue_families_;
    std::string gpu_name_;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;

    // Synchronization follows the vk-bootstrap triangle example:
    // - acquire semaphores + fences are per frame in flight,
    // - present semaphores are per swapchain image,
    // - image_in_flight_ tracks which fence last used each swapchain image.
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<VkSemaphore> available_semaphores_; // per frame
    std::vector<VkFence> in_flight_fences_;         // per frame
    std::vector<VkSemaphore> finished_semaphores_;  // per swapchain image
    std::vector<VkFence> image_in_flight_;          // per swapchain image
    uint32_t current_frame_ = 0;
    bool pending_resize_ = false;

    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
