#include <wlvk/wlvk.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Some ffmpeg installs ship headers without C++ guards; wrap defensively.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include "shaders_spirv.h"

namespace {

constexpr uint32_t kBufferCount = 3;
constexpr VkDeviceSize kPlaneAlign = 256;

void ck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed");
    }
}

void ff(int err, const char* what) {
    if (err < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(err, buf, sizeof(buf));
        throw std::runtime_error(std::string(what) + ": " + buf);
    }
}

bool decode_next(AVFormatContext* fmt, AVCodecContext* codec, AVPacket* pkt, AVFrame* frame,
                 int video_stream) {
    for (;;) {
        int r = avcodec_receive_frame(codec, frame);
        if (r == 0) {
            return true;
        }
        if (r == AVERROR_EOF) {
            return false;
        }
        if (r != AVERROR(EAGAIN)) {
            ff(r, "avcodec_receive_frame");
        }

        r = av_read_frame(fmt, pkt);
        if (r < 0) {
            ff(avcodec_send_packet(codec, nullptr), "flush avcodec_send_packet");
            continue;
        }
        if (pkt->stream_index == video_stream) {
            ff(avcodec_send_packet(codec, pkt), "avcodec_send_packet");
        }
        av_packet_unref(pkt);
    }
}

VkSamplerYcbcrModelConversion pick_model(const AVFrame* frame, uint32_t height) {
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_FCC:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
    default:
        // Stream didn't say; guess from resolution.
        return height >= 720 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709
                             : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <video-file>\n", argv[0]);
        return 1;
    }
    try {
        av_log_set_level(AV_LOG_ERROR);

        AVFormatContext* format = nullptr;
        ff(avformat_open_input(&format, argv[1], nullptr, nullptr), "avformat_open_input");
        ff(avformat_find_stream_info(format, nullptr), "avformat_find_stream_info");

        const AVCodec* decoder = nullptr;
        const int video_stream =
            av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        ff(video_stream, "no video stream");

        AVStream* stream = format->streams[video_stream];
        AVCodecContext* codec = avcodec_alloc_context3(decoder);
        ff(avcodec_parameters_to_context(codec, stream->codecpar),
           "avcodec_parameters_to_context");
        ff(avcodec_open2(codec, decoder, nullptr), "avcodec_open2");

        if (codec->pix_fmt != AV_PIX_FMT_YUV420P && codec->pix_fmt != AV_PIX_FMT_YUVJ420P) {
            const char* name = av_get_pix_fmt_name(codec->pix_fmt);
            throw std::runtime_error(std::string("only 8-bit 4:2:0 supported, got ")
                                     + (name ? name : "?"));
        }
        if ((codec->width % 2) != 0 || (codec->height % 2) != 0) {
            throw std::runtime_error("odd-dimensioned 4:2:0 video is not supported");
        }

        const uint32_t src_w = static_cast<uint32_t>(codec->width);
        const uint32_t src_h = static_cast<uint32_t>(codec->height);

        // Per-slot staging buffers and scanout image views. Declared before the
        // window so on_resize can invalidate the views: a resize rebuilds the
        // underlying scanout images.
        struct Slot {
            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation staging_alloc = nullptr;
            void* mapped = nullptr;
            VkDeviceSize bytes = 0;
            VkImageView view = VK_NULL_HANDLE;
            bool view_valid = false;
        };
        std::vector<Slot> slots(kBufferCount);

        wlvk::WindowConfig config;
        config.title = "wlvk-video";
        config.app_id = "wlvk-video";
        config.enable_validation = true; // TEMP
        config.width = src_w;
        config.height = src_h;
        // Frames are written by a graphics pipeline now, not a transfer.
        config.image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        config.on_resize = [&](uint32_t, uint32_t) {
            for (Slot& s : slots) {
                s.view_valid = false;
            }
        };

        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_features = {};
        ycbcr_features.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        ycbcr_features.samplerYcbcrConversion = VK_TRUE;
        config.configure_device = [&ycbcr_features](wlvk::DeviceBuilder& builder) {
            builder.chain_next(&ycbcr_features);
        };

        wlvk::Window win{config};

        // ---- decode the first frame: needed for strides and color metadata
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (!decode_next(format, codec, pkt, frame, video_stream)) {
            throw std::runtime_error("video stream has no decodable frames");
        }

        // ---- YCbCr conversion, sampler, multiplanar target image
        const VkFormat yuv_format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;

        VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
        VkSamplerYcbcrConversionCreateInfo conv_info = {};
        conv_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
        conv_info.format = yuv_format;
        conv_info.ycbcrModel = pick_model(frame, src_h);
        conv_info.ycbcrRange = frame->color_range == AVCOL_RANGE_JPEG
                                   ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                                   : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        conv_info.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conv_info.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conv_info.chromaFilter = VK_FILTER_LINEAR;
        std::fprintf(stderr, "TEMP model=%d range=%d\n", int(conv_info.ycbcrModel),
                     int(conv_info.ycbcrRange)); // TEMP
        ck(vkCreateSamplerYcbcrConversion(win.device(), &conv_info, nullptr, &conversion),
           "vkCreateSamplerYcbcrConversion");

        VkSamplerYcbcrConversionInfo conv_ref = {};
        conv_ref.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        conv_ref.conversion = conversion;

        VkImage yuv_image = VK_NULL_HANDLE;
        VmaAllocation yuv_alloc = nullptr;
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = yuv_format;
        image_info.extent = {src_w, src_h, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Sampled multi-planar images require the mutable format flag.
        image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo yuv_alloc_info = {};
        yuv_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        ck(vmaCreateImage(win.allocator(), &image_info, &yuv_alloc_info, &yuv_image,
                          &yuv_alloc, nullptr),
           "vmaCreateImage(yuv)");

        VkImageView yuv_view = VK_NULL_HANDLE;
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.pNext = &conv_ref;
        view_info.image = yuv_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = yuv_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.layerCount = 1;
        view_info.subresourceRange.levelCount = 1;
        ck(vkCreateImageView(win.device(), &view_info, nullptr, &yuv_view),
           "vkCreateImageView(yuv)");

        VkSampler sampler = VK_NULL_HANDLE;
        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.pNext = &conv_ref;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ck(vkCreateSampler(win.device(), &sampler_info, nullptr, &sampler),
           "vkCreateSampler");

        // ---- descriptors
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // Sampler YCbCr conversion requires the sampler to be immutable in the
        // set layout; per-set sampler fields are ignored for such bindings.
        binding.pImmutableSamplers = &sampler;

        VkDescriptorSetLayoutCreateInfo dsl_info = {};
        dsl_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl_info.bindingCount = 1;
        dsl_info.pBindings = &binding;
        VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
        ck(vkCreateDescriptorSetLayout(win.device(), &dsl_info, nullptr, &desc_layout),
           "vkCreateDescriptorSetLayout");

        VkDescriptorPoolSize pool_size = {};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = 1;
        VkDescriptorPoolCreateInfo dp_info = {};
        dp_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp_info.maxSets = 1;
        dp_info.poolSizeCount = 1;
        dp_info.pPoolSizes = &pool_size;
        VkDescriptorPool desc_pool = VK_NULL_HANDLE;
        ck(vkCreateDescriptorPool(win.device(), &dp_info, nullptr, &desc_pool),
           "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo ds_alloc = {};
        ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ds_alloc.descriptorPool = desc_pool;
        ds_alloc.descriptorSetCount = 1;
        ds_alloc.pSetLayouts = &desc_layout;
        VkDescriptorSet desc_set = VK_NULL_HANDLE;
        ck(vkAllocateDescriptorSets(win.device(), &ds_alloc, &desc_set),
           "vkAllocateDescriptorSets");

        VkDescriptorImageInfo image_desc = {};
        image_desc.imageView = yuv_view;
        image_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet desc_write = {};
        desc_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        desc_write.dstSet = desc_set;
        desc_write.dstBinding = 0;
        desc_write.descriptorCount = 1;
        desc_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        desc_write.pImageInfo = &image_desc;
        vkUpdateDescriptorSets(win.device(), 1, &desc_write, 0, nullptr);

        // ---- command objects
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

        // ---- per-slot upload staging + scanout image views
        auto destroy_slot_staging = [&](Slot& s) {
            if (s.staging != VK_NULL_HANDLE) {
                vmaDestroyBuffer(win.allocator(), s.staging, s.staging_alloc);
                s.staging = VK_NULL_HANDLE;
                s.mapped = nullptr;
            }
        };
        auto destroy_slot_view = [&](Slot& s) {
            if (s.view != VK_NULL_HANDLE) {
                vkDestroyImageView(win.device(), s.view, nullptr);
                s.view = VK_NULL_HANDLE;
            }
            s.view_valid = false;
        };

        // Plane packing in the staging buffer (rows keep their source stride).
        const int32_t ls0 = frame->linesize[0];
        const int32_t ls1 = frame->linesize[1];
        const int32_t ls2 = frame->linesize[2];
        const uint32_t chroma_h = src_h / 2;
        const VkDeviceSize off0 = 0;
        const VkDeviceSize off1 = (off0 + ls0 * src_h + kPlaneAlign - 1) & ~(kPlaneAlign - 1);
        const VkDeviceSize off2 = (off1 + ls1 * chroma_h + kPlaneAlign - 1) & ~(kPlaneAlign - 1);
        const VkDeviceSize staging_bytes = off2 + ls2 * chroma_h;

        auto ensure_staging = [&](Slot& s) {
            if (s.staging != VK_NULL_HANDLE && s.bytes >= staging_bytes) {
                return;
            }
            destroy_slot_staging(s);
            VkBufferCreateInfo buffer_info = {};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = staging_bytes;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo alloc_info = {};
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
            alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                               | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo out = {};
            ck(vmaCreateBuffer(win.allocator(), &buffer_info, &alloc_info, &s.staging,
                               &s.staging_alloc, &out),
               "vmaCreateBuffer(staging)");
            s.mapped = out.pMappedData;
            s.bytes = staging_bytes;
        };

        // ---- pipeline (created on the first frame once the scanout format is
        // known)
        VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        auto create_pipeline = [&](VkFormat scanout_format) {
            VkPipelineLayoutCreateInfo pl_info = {};
            pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl_info.setLayoutCount = 1;
            pl_info.pSetLayouts = &desc_layout;
            ck(vkCreatePipelineLayout(win.device(), &pl_info, nullptr, &pipe_layout),
               "vkCreatePipelineLayout");

            auto make_module = [&](std::span<const uint8_t> code, const char* what) {
                VkShaderModuleCreateInfo sm_info = {};
                sm_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                sm_info.codeSize = code.size_bytes();
                sm_info.pCode = reinterpret_cast<const uint32_t*>(code.data());
                VkShaderModule module = VK_NULL_HANDLE;
                ck(vkCreateShaderModule(win.device(), &sm_info, nullptr, &module), what);
                return module;
            };
            VkShaderModule vert = make_module(wlvk_shaders::tri_vert_spv, "vert module");
            VkShaderModule frag = make_module(wlvk_shaders::yuv_frag_spv, "frag module");

            VkPipelineShaderStageCreateInfo stages[2] = {};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vert;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = frag;
            stages[1].pName = "main";

            VkPipelineVertexInputStateCreateInfo vertex_input = {};
            vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo assembly = {};
            assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo viewport_state = {};
            viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport_state.viewportCount = 1;
            viewport_state.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo raster = {};
            raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisample = {};
            multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineColorBlendAttachmentState blend_att = {};
            blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo blend = {};
            blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            blend.attachmentCount = 1;
            blend.pAttachments = &blend_att;

            const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic = {};
            dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = dynamics;

            VkPipelineRenderingCreateInfo rendering = {};
            rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &scanout_format;

            VkGraphicsPipelineCreateInfo gp_info = {};
            gp_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gp_info.pNext = &rendering;
            gp_info.stageCount = 2;
            gp_info.pStages = stages;
            gp_info.pVertexInputState = &vertex_input;
            gp_info.pInputAssemblyState = &assembly;
            gp_info.pViewportState = &viewport_state;
            gp_info.pRasterizationState = &raster;
            gp_info.pMultisampleState = &multisample;
            gp_info.pColorBlendState = &blend;
            gp_info.pDynamicState = &dynamic;
            gp_info.layout = pipe_layout;
            ck(vkCreateGraphicsPipelines(win.device(), VK_NULL_HANDLE, 1, &gp_info, nullptr,
                                         &pipeline),
               "vkCreateGraphicsPipelines");

            vkDestroyShaderModule(win.device(), vert, nullptr);
            vkDestroyShaderModule(win.device(), frag, nullptr);
        };

        // ---- main loop
        bool eof = false;
        wlvk::Frame f;
        // Track the target image's layout across frames: previous slots may still
        // be sampling it while a new upload begins.
        VkImageLayout yuv_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        while (!eof && win.wait_frame(f)) {
            if (pipeline == VK_NULL_HANDLE) {
                create_pipeline(f.format);
            }
            if (!slots[f.index].view_valid) {
                destroy_slot_view(slots[f.index]);
                VkImageViewCreateInfo sv_info = {};
                sv_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                sv_info.image = f.image;
                sv_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                sv_info.format = f.format;
                sv_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                sv_info.subresourceRange.layerCount = 1;
                sv_info.subresourceRange.levelCount = 1;
                ck(vkCreateImageView(win.device(), &sv_info, nullptr,
                                     &slots[f.index].view),
                   "vkCreateImageView(scanout)");
                slots[f.index].view_valid = true;
            }

            ensure_staging(slots[f.index]);
            uint8_t* map = static_cast<uint8_t*>(slots[f.index].mapped);
            for (uint32_t y = 0; y < src_h; ++y) {
                std::memcpy(map + off0 + static_cast<VkDeviceSize>(y) * ls0,
                            frame->data[0] + static_cast<size_t>(y) * frame->linesize[0],
                            static_cast<size_t>(src_w));
            }
            for (uint32_t y = 0; y < chroma_h; ++y) {
                std::memcpy(map + off1 + static_cast<VkDeviceSize>(y) * ls1,
                            frame->data[1] + static_cast<size_t>(y) * frame->linesize[1],
                            static_cast<size_t>(src_w / 2));
                std::memcpy(map + off2 + static_cast<VkDeviceSize>(y) * ls2,
                            frame->data[2] + static_cast<size_t>(y) * frame->linesize[2],
                            static_cast<size_t>(src_w / 2));
            }

            VkCommandBuffer cb = command_buffers[f.index];
            ck(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
            VkCommandBufferBeginInfo begin = {};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            ck(vkBeginCommandBuffer(cb, &begin), "vkBeginCommandBuffer");

            // Upload: wait for prior frames' sampling to finish before overwriting.
            VkPipelineStageFlags src_stage;
            VkImageMemoryBarrier to_dst = {};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            if (yuv_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            } else {
                src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                to_dst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_dst.oldLayout = yuv_layout;
            }
            const VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.image = yuv_image;
            to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT
                                                 | VK_IMAGE_ASPECT_PLANE_1_BIT
                                                 | VK_IMAGE_ASPECT_PLANE_2_BIT;
            to_dst.subresourceRange.layerCount = 1;
            to_dst.subresourceRange.levelCount = 1;
            vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr,
                                 1, &to_dst);

            VkBufferImageCopy regions[3] = {};
            const VkDeviceSize offs[3] = {off0, off1, off2};
            const int32_t strides[3] = {ls0, ls1, ls2};
            const uint32_t widths[3] = {src_w, src_w / 2, src_w / 2};
            const VkImageAspectFlagBits aspects[3] = {
                VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT,
                VK_IMAGE_ASPECT_PLANE_2_BIT};
            const uint32_t heights[3] = {src_h, chroma_h, chroma_h};
            for (uint32_t p = 0; p < 3; ++p) {
                regions[p].bufferOffset = offs[p];
                regions[p].bufferRowLength = static_cast<uint32_t>(strides[p]);
                regions[p].bufferImageHeight = heights[p];
                regions[p].imageSubresource.aspectMask = aspects[p];
                regions[p].imageSubresource.layerCount = 1;
                regions[p].imageExtent = {widths[p], heights[p], 1};
            }
            vkCmdCopyBufferToImage(cb, slots[f.index].staging, yuv_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 3, regions);

            VkImageMemoryBarrier to_shader = to_dst;
            to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_shader.newLayout = yuv_layout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &to_shader);

            VkImageMemoryBarrier to_color = {};
            to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_color.image = f.image;
            to_color.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_color.subresourceRange.layerCount = 1;
            to_color.subresourceRange.levelCount = 1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &to_color);

            VkRenderingAttachmentInfo color_att = {};
            color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color_att.imageView = slots[f.index].view;
            color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo render_info = {};
            render_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            render_info.renderArea.extent = {win.width(), win.height()};
            render_info.layerCount = 1;
            render_info.colorAttachmentCount = 1;
            render_info.pColorAttachments = &color_att;
            vkCmdBeginRendering(cb, &render_info);

            VkViewport viewport = {};
            viewport.width = static_cast<float>(win.width());
            viewport.height = static_cast<float>(win.height());
            viewport.maxDepth = 1.0f;
            VkRect2D scissor = {{0, 0}, {win.width(), win.height()}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);

            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1,
                                    &desc_set, 0, nullptr);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);

            VkImageMemoryBarrier to_general = {};
            to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_general.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_general.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.image = f.image;
            to_general.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_general.subresourceRange.layerCount = 1;
            to_general.subresourceRange.levelCount = 1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &to_general);

            ck(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

            VkSubmitInfo submit = {};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cb;
            ck(vkQueueSubmit(win.graphics_queue(), 1, &submit, f.render_fence),
               "vkQueueSubmit");

            win.present(f);

            if (!decode_next(format, codec, pkt, frame, video_stream)) {
                eof = true;
            }
        }

        ck(vkDeviceWaitIdle(win.device()), "vkDeviceWaitIdle");
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(win.device(), pipeline, nullptr);
        }
        if (pipe_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(win.device(), pipe_layout, nullptr);
        }
        vkDestroyDescriptorPool(win.device(), desc_pool, nullptr);
        vkDestroyDescriptorSetLayout(win.device(), desc_layout, nullptr);
        for (Slot& s : slots) {
            destroy_slot_view(s);
            destroy_slot_staging(s);
        }
        vkDestroySampler(win.device(), sampler, nullptr);
        vkDestroySamplerYcbcrConversion(win.device(), conversion, nullptr);
        vkDestroyImageView(win.device(), yuv_view, nullptr);
        vmaDestroyImage(win.allocator(), yuv_image, yuv_alloc);
        vkDestroyCommandPool(win.device(), command_pool, nullptr);

        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
