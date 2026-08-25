#include "detail.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wlvk::detail {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult "
             + std::to_string(static_cast<int>(result)) + ")");
    }
}

const char* const kRequiredDeviceExts[4] = {
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_KHR_external_fence_fd",
    "VK_EXT_image_drm_format_modifier",
};

VkFormat vk_format_for_fourcc(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

Impl::Impl(const WindowConfig& config) : config_(config) {
    init_wayland();
    init_vulkan();
}

static VkFormatFeatureFlags2 feature_bits_for_usage(VkImageUsageFlags usage) {
    VkFormatFeatureFlags2 bits = 0;
    if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
        bits |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
        bits |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) {
        bits |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) {
        bits |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
        bits |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
    }
    return bits;
}

ModifierSupport& Impl::modifier_support(VkFormat format) {
    for (ModifierSupport& entry : modifier_cache_) {
        if (entry.format == format) {
            return entry;
        }
    }

    ModifierSupport entry;
    entry.format = format;

    VkDrmFormatModifierPropertiesListEXT mod_list = {};
    mod_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

    VkFormatProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props.pNext = &mod_list;
    vkGetPhysicalDeviceFormatProperties2(physical_device_, format, &props);

    if (mod_list.drmFormatModifierCount == 0) {
        WLVK_TRACE(*this, "driver reports no DRM format modifiers; accepting compositor "
                          "choice blindly");
        entry.blind = true;
    } else {
        std::vector<VkDrmFormatModifierPropertiesEXT> mods(mod_list.drmFormatModifierCount);
        mod_list.pDrmFormatModifierProperties = mods.data();
        vkGetPhysicalDeviceFormatProperties2(physical_device_, format, &props);

        const VkFormatFeatureFlags2 required = feature_bits_for_usage(config_.image_usage);
        for (const auto& m : mods) {
            if ((m.drmFormatModifierTilingFeatures & required) == required) {
                entry.modifiers.push_back(m.drmFormatModifier);
            }
        }
    }

    modifier_cache_.push_back(std::move(entry));
    return modifier_cache_.back();
}

VkBool32 Callbacks::debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                   VkDebugUtilsMessageTypeFlagsEXT,
                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::fprintf(stderr, "validation [%s]: %s\n",
                 severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                 data->pMessage);
    return VK_FALSE;
}

void Impl::create_instance() {
    std::vector<const char*> extensions = {
        "VK_KHR_surface",
        "VK_KHR_wayland_surface",
    };
    extensions.insert(extensions.end(), config_.extra_instance_extensions.begin(),
                      config_.extra_instance_extensions.end());

    std::vector<const char*> layers;
    if (config_.enable_validation) {
        uint32_t layer_count = 0;
        check_vk(vkEnumerateInstanceLayerProperties(&layer_count, nullptr),
                 "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> available(layer_count);
        check_vk(vkEnumerateInstanceLayerProperties(&layer_count, available.data()),
                 "vkEnumerateInstanceLayerProperties");
        bool found = false;
        for (const auto& layer : available) {
            if (std::strcmp(layer.layerName, kValidationLayer) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            fail("validation requested but layer unavailable");
        }
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    layers.insert(layers.end(), config_.extra_instance_layers.begin(),
                  config_.extra_instance_layers.end());

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = config_.app_id;
    app_info.apiVersion = config_.api_version;

    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app_info;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    check_vk(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
    volkLoadInstanceOnly(instance_);

    if (!layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = {};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = &Callbacks::debug_callback;
        check_vk(vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr,
                                                &debug_messenger_),
                 "vkCreateDebugUtilsMessengerEXT");
    }
}

void Impl::pick_physical_device() {
    struct CandidateData {
        VkPhysicalDevice device = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props = {};
        uint32_t graphics_family = 0;
        uint32_t present_family = 0;
        std::vector<VkQueueFamilyProperties> families;
        std::vector<VkExtensionProperties> extensions;
    };

    uint32_t count = 0;
    check_vk(vkEnumeratePhysicalDevices(instance_, &count, nullptr),
             "vkEnumeratePhysicalDevices");
    if (count == 0) {
        fail("no Vulkan devices");
    }
    std::vector<VkPhysicalDevice> devices(count);
    check_vk(vkEnumeratePhysicalDevices(instance_, &count, devices.data()),
             "vkEnumeratePhysicalDevices");

    std::vector<CandidateData> capable;
    for (VkPhysicalDevice candidate : devices) {
        CandidateData data;
        data.device = candidate;

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        data.families.resize(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
                                                 data.families.data());

        uint32_t ext_count = 0;
        check_vk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ext_count, nullptr),
                 "vkEnumerateDeviceExtensionProperties");
        data.extensions.resize(ext_count);
        check_vk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ext_count,
                                                      data.extensions.data()),
                 "vkEnumerateDeviceExtensionProperties");

        vkGetPhysicalDeviceProperties(candidate, &data.props);

        bool all_exts = true;
        for (const char* want : kRequiredDeviceExts) {
            bool found = false;
            for (const auto& ext : data.extensions) {
                if (std::strcmp(ext.extensionName, want) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                all_exts = false;
                break;
            }
        }
        if (!all_exts) {
            continue;
        }

        bool have_graphics = false;
        bool have_present = false;
        for (uint32_t i = 0; i < family_count && !(have_graphics && have_present); ++i) {
            if ((data.families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && !have_graphics) {
                data.graphics_family = i;
                have_graphics = true;
            }
            if (vkGetPhysicalDeviceWaylandPresentationSupportKHR(candidate, i, display_)
                && !have_present) {
                data.present_family = i;
                have_present = true;
            }
        }
        if (!have_graphics || !have_present) {
            continue;
        }

        capable.push_back(std::move(data));
    }

    if (capable.empty()) {
        fail("no device with graphics+present and external memory/fence support");
    }

    std::vector<DeviceCandidate> candidates(capable.size());
    for (size_t i = 0; i < capable.size(); ++i) {
        candidates[i].device = capable[i].device;
        candidates[i].properties = &capable[i].props;
        candidates[i].queue_families = capable[i].families;
        candidates[i].extensions = capable[i].extensions;
    }

    uint32_t chosen_index = 0;
    if (config_.select_device != nullptr) {
        chosen_index = config_.select_device(candidates);
        if (chosen_index >= candidates.size()) {
            fail("select_device rejected every candidate");
        }
    }

    physical_device_ = capable[chosen_index].device;
    graphics_family_ = capable[chosen_index].graphics_family;
    present_family_ = capable[chosen_index].present_family;
    gpu_name_ = capable[chosen_index].props.deviceName;

    for (const auto& ext : capable[chosen_index].extensions) {
        if (std::strcmp(ext.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
            budget_ext_ = true;
            break;
        }
    }
}

void Impl::query_default_feedback() {
    default_feedback_ = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf_);
    if (default_feedback_ == nullptr) {
        fail("get_default_feedback failed");
    }
    zwp_linux_dmabuf_feedback_v1_add_listener(default_feedback_, &feedback_listener_, this);
    if (wl_display_roundtrip(display_) < 0 || wl_display_roundtrip(display_) < 0) {
        fail("wayland roundtrip during feedback failed");
    }
    resolve_format();
}

void Impl::resolve_format() {
    if (offers_.empty()) {
        fail("compositor feedback offers no dmabuf formats");
    }

    driver_ok_.clear();
    for (const FormatOffer& offer : offers_) {
        if (offer.vk_format == VK_FORMAT_UNDEFINED) {
            continue;
        }
        const ModifierSupport& sup = modifier_support(offer.vk_format);
        const bool supported =
            sup.blind
            || std::find(sup.modifiers.begin(), sup.modifiers.end(), offer.modifier)
                   != sup.modifiers.end();
        if (supported) {
            driver_ok_.push_back(offer);
        }
    }

    std::optional<FormatOffer> chosen;
    if (config_.choose_format != nullptr) {
        chosen = config_.choose_format(offers_, driver_ok_);
    } else if (!driver_ok_.empty()) {
        chosen = driver_ok_.front();
    }
    if (!chosen.has_value()) {
        fail("no compositor-offered dmabuf format is usable with the configured image usage");
    }

    bool known = false;
    for (const FormatOffer& offer : offers_) {
        if (offer.drm_fourcc == chosen->drm_fourcc && offer.modifier == chosen->modifier
            && offer.vk_format == chosen->vk_format) {
            known = true;
            break;
        }
    }
    if (!known) {
        fail("choose_format returned an offer that was not in the compositor list");
    }

    chosen_fourcc_ = chosen->drm_fourcc;
    chosen_vk_format_ = chosen->vk_format;
    chosen_modifier_ = chosen->modifier;

    char hex[32];
    std::snprintf(hex, sizeof(hex), "%llx", static_cast<unsigned long long>(chosen_modifier_));
    log("chosen modifier 0x" + std::string(hex));
}

void Impl::create_device() {
    std::vector<const char*> extensions(std::begin(kRequiredDeviceExts),
                                        std::end(kRequiredDeviceExts));
    if (budget_ext_) {
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    // Dynamic rendering is the baseline path for drawing into scanout images;
    // enable it up front so renderPass-less pipelines are always legal.
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_features = {};
    dynamic_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic_features.dynamicRendering = VK_TRUE;
    void* pnext_head = &dynamic_features;
    DeviceBuilder builder(physical_device_, &extensions, &pnext_head);
    if (config_.configure_device != nullptr) {
        config_.configure_device(builder);
    }

    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    std::vector<uint32_t> families;
    if (graphics_family_ == present_family_) {
        families.push_back(graphics_family_);
    } else {
        families.push_back(graphics_family_);
        families.push_back(present_family_);
    }
    for (uint32_t family : families) {
        VkDeviceQueueCreateInfo qi = {};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queue_infos.push_back(qi);
    }

    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = pnext_head;
    info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    info.pQueueCreateInfos = queue_infos.data();
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check_vk(vkCreateDevice(physical_device_, &info, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphics_family_, 0, &queue_);
}

void Impl::create_allocator() {
    VmaAllocatorCreateInfo info = {};
    info.flags = VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
    if (budget_ext_) {
        info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    info.instance = instance_;
    info.physicalDevice = physical_device_;
    info.device = device_;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    VmaVulkanFunctions functions = {};
    check_vk(vmaImportVulkanFunctionsFromVolk(&info, &functions),
             "vmaImportVulkanFunctionsFromVolk");
    info.pVulkanFunctions = &functions;
    check_vk(vmaCreateAllocator(&info, &allocator_), "vmaCreateAllocator");
}

void Impl::open_drm_node() {
    if (main_device_ != 0) {
        drmDevicePtr devices[16] = {};
        const int count = drmGetDevices2(0, devices, 16);
        for (int i = 0; i < count; ++i) {
            const drmDevicePtr dev = devices[i];
            const char* matched = nullptr;
            for (int t = 0; t < DRM_NODE_MAX && matched == nullptr; ++t) {
                if ((dev->available_nodes & (1u << t)) == 0) {
                    continue;
                }
                struct stat st = {};
                if (::stat(dev->nodes[t], &st) == 0 && st.st_rdev == main_device_) {
                    matched = dev->nodes[t];
                }
            }
            if (matched == nullptr) {
                continue;
            }
            const bool have_render = (dev->available_nodes & (1u << DRM_NODE_RENDER)) != 0;
            const char* node = have_render ? dev->nodes[DRM_NODE_RENDER] : matched;
            drm_fd_ = ::open(node, O_RDWR | O_CLOEXEC);
            if (drm_fd_ >= 0) {
                WLVK_TRACE(*this, std::string("opened drm node ") + node + " (matched "
                                           + matched + " via dmabuf main_device)");
                break;
            }
        }
        if (count > 0) {
            drmFreeDevices(devices, count);
        }
    }
    if (drm_fd_ < 0) {
        WLVK_TRACE(*this, main_device_ != 0
                              ? "no drm device matched main_device; using fallback node"
                              : "main_device unknown; using fallback node");
        drm_fd_ = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    }
    if (drm_fd_ < 0) {
        fail("failed to open a usable DRM render node");
    }
}

uint32_t Impl::create_syncobj() {
    uint32_t handle = 0;
    if (drmSyncobjCreate(drm_fd_, 0, &handle) != 0) {
        fail("drmSyncobjCreate failed");
    }
    return handle;
}

wp_linux_drm_syncobj_timeline_v1* Impl::share_syncobj(uint32_t syncobj, int* out_fd) {
    int fd = -1;
    if (drmSyncobjHandleToFD(drm_fd_, syncobj, &fd) != 0) {
        fail("drmSyncobjHandleToFD failed");
    }
    *out_fd = fd;
    return wp_linux_drm_syncobj_manager_v1_import_timeline(syncobj_manager_, fd);
}

void Impl::materialize_acquire_point(uint32_t syncobj, uint64_t point, VkFence fence) {
    VkFenceGetFdInfoKHR get_fd_info = {};
    get_fd_info.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
    get_fd_info.fence = fence;
    get_fd_info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    int sync_fd = -1;
    check_vk(vkGetFenceFdKHR(device_, &get_fd_info, &sync_fd), "vkGetFenceFdKHR");
    if (sync_fd < 0) {
        fail("vkGetFenceFdKHR returned no sync fd");
    }

    uint32_t tmp = 0;
    if (drmSyncobjCreate(drm_fd_, 0, &tmp) != 0
        || drmSyncobjImportSyncFile(drm_fd_, tmp, sync_fd) != 0
        || drmSyncobjTransfer(drm_fd_, syncobj, point, tmp, 0, 0) != 0) {
        ::close(sync_fd);
        if (tmp != 0) {
            drmSyncobjDestroy(drm_fd_, tmp);
        }
        fail("failed to materialize acquire point from sync fd");
    }
    drmSyncobjDestroy(drm_fd_, tmp);
    ::close(sync_fd);
}

bool Impl::wait_release_point(uint32_t syncobj, uint64_t point, int64_t timeout_ns) {
    uint32_t first = 0;
    if (drmSyncobjTimelineWait(drm_fd_, &syncobj, &point, 1, timeout_ns, 0, &first) != 0) {
        return false;
    }
    return true;
}

// ---- dmabuf feedback listeners ----

void Callbacks::feedback_main_device(void* data, zwp_linux_dmabuf_feedback_v1*,
                                     wl_array* device) {
    auto* self = static_cast<Impl*>(data);
    if (device->size >= sizeof(dev_t)) {
        std::memcpy(&self->main_device_, device->data, sizeof(dev_t));
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%lx", static_cast<unsigned long>(self->main_device_));
        self->log("dmabuf main_device dev_t=0x" + std::string(hex));
    }
}

void Callbacks::feedback_format_table(void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd,
                                      uint32_t size) {
    auto* self = static_cast<Impl*>(data);
    self->format_table_.resize(size);
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(size)) {
        const ssize_t n = pread(fd, self->format_table_.data() + total, size - total, total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    ::close(fd);
}

void Callbacks::feedback_tranche_formats(void* data, zwp_linux_dmabuf_feedback_v1*,
                                         wl_array* indices) {
    auto* self = static_cast<Impl*>(data);
    const auto* idx = static_cast<const uint16_t*>(indices->data);
    const size_t count = indices->size / sizeof(uint16_t);
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = static_cast<size_t>(idx[i]) * 16;
        if (offset + 16 > self->format_table_.size()) {
            continue;
        }
        uint32_t fourcc = 0;
        uint64_t modifier = 0;
        std::memcpy(&fourcc, self->format_table_.data() + offset, 4);
        std::memcpy(&modifier, self->format_table_.data() + offset + 8, 8);
        self->tranche_candidates_.push_back({fourcc, modifier});
    }
}

void Callbacks::feedback_tranche_done(void* data, zwp_linux_dmabuf_feedback_v1*) {
    auto* self = static_cast<Impl*>(data);
    if (self->offers_.empty()) {
        for (const auto& [fourcc, modifier] : self->tranche_candidates_) {
            self->offers_.push_back({fourcc, vk_format_for_fourcc(fourcc), modifier});
        }
    }
    self->tranche_candidates_.clear();
}

}  // namespace wlvk::detail

namespace wlvk {

void DeviceBuilder::add_extensions(std::span<const char* const> extensions) {
    for (const char* extension : extensions) {
        bool present = false;
        for (const char* existing : *extensions_) {
            if (std::strcmp(existing, extension) == 0) {
                present = true;
                break;
            }
        }
        if (!present) {
            extensions_->push_back(extension);
        }
    }
}

void DeviceBuilder::chain_next(void* structure) {
    auto* node = static_cast<VkBaseOutStructure*>(structure);
    auto* slot = reinterpret_cast<VkBaseOutStructure**>(pnext_head_);
    while (*slot != nullptr) {
        slot = &(*slot)->pNext;
    }
    *slot = node;
}

}  // namespace wlvk
