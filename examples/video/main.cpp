#include <wlvk/wlvk.hpp>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// Some ffmpeg installs ship headers without C++ guards; wrap defensively.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr uint32_t kBufferCount = 3;

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

struct Upload {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    VkDeviceSize pitch = 0;
};

void destroy_upload(wlvk::Window& win, Upload& up) {
    if (up.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(win.allocator(), up.buffer, up.allocation);
        up = {};
    }
}

// Staging buffers are keyed on the scanout geometry, which is only known once
// wait_frame() hands out a slot - and changes on compositor resizes.
void ensure_upload(wlvk::Window& win, Upload& up, uint32_t w, uint32_t h,
                   VkDeviceSize pitch) {
    if (up.buffer != VK_NULL_HANDLE && up.width == w && up.height == h && up.pitch == pitch) {
        return;
    }
    destroy_upload(win, up);

    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = pitch * h;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                       | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo out = {};
    ck(vmaCreateBuffer(win.allocator(), &buffer_info, &alloc_info, &up.buffer,
                       &up.allocation, &out),
       "vmaCreateBuffer(staging)");
    up.mapped = out.pMappedData;
    up.width = w;
    up.height = h;
    up.pitch = pitch;
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

        const uint32_t src_w = static_cast<uint32_t>(codec->width);
        const uint32_t src_h = static_cast<uint32_t>(codec->height);

        wlvk::WindowConfig config;
        config.title = "wlvk-video";
        config.app_id = "wlvk-video";
        config.width = src_w;
        config.height = src_h;
        wlvk::Window win{config};

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

        SwsContext* sws = nullptr;
        uint32_t sws_w = 0;
        uint32_t sws_h = 0;
        AVPixelFormat sws_output_fmt = AV_PIX_FMT_NONE;
        std::vector<Upload> uploads(kBufferCount);

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        wlvk::Frame frame_out;
        bool eof = false;
        while (!eof && win.wait_frame(frame_out)) {
            if (!decode_next(format, codec, pkt, frame, video_stream)) {
                eof = true;
                continue;
            }

            const uint32_t dst_w = win.width();
            const uint32_t dst_h = win.height();

            AVPixelFormat dst_fmt;
            switch (frame_out.format) {
            case VK_FORMAT_B8G8R8A8_UNORM:
                dst_fmt = AV_PIX_FMT_BGRA;
                break;
            case VK_FORMAT_R8G8B8A8_UNORM:
                dst_fmt = AV_PIX_FMT_RGBA;
                break;
            default:
                throw std::runtime_error("unsupported scanout format "
                                         + std::to_string(frame_out.format));
            }

            if (sws == nullptr || sws_w != dst_w || sws_h != dst_h || sws_output_fmt != dst_fmt) {
                sws_freeContext(sws);
                sws = sws_getContext(src_w, src_h, codec->pix_fmt, static_cast<int>(dst_w),
                                     static_cast<int>(dst_h), dst_fmt, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
                if (sws == nullptr) {
                    const char* name = av_get_pix_fmt_name(codec->pix_fmt);
                    throw std::runtime_error(std::string("sws_getContext failed for ")
                                             + (name ? name : "?"));
                }
                sws_w = dst_w;
                sws_h = dst_h;
                sws_output_fmt = dst_fmt;
            }

            Upload& up = uploads[frame_out.index];
            ensure_upload(win, up, dst_w, dst_h, frame_out.pitch);

            uint8_t* dst[4] = {static_cast<uint8_t*>(up.mapped), nullptr, nullptr, nullptr};
            const int dst_linesize[4] = {static_cast<int>(frame_out.pitch), 0, 0, 0};
            sws_scale(sws, frame->data, frame->linesize, 0, codec->height, dst,
                      dst_linesize);

            VkCommandBuffer cb = command_buffers[frame_out.index];
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
            to_transfer.image = frame_out.image;
            to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_transfer.subresourceRange.levelCount = 1;
            to_transfer.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_transfer);

            VkBufferImageCopy region = {};
            region.bufferRowLength = static_cast<uint32_t>(frame_out.pitch / 4);
            region.bufferImageHeight = dst_h;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {dst_w, dst_h, 1};
            vkCmdCopyBufferToImage(cb, up.buffer, frame_out.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                   &region);

            ck(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

            VkSubmitInfo submit = {};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cb;
            ck(vkQueueSubmit(win.graphics_queue(), 1, &submit, frame_out.render_fence),
               "vkQueueSubmit");

            win.present(frame_out);
        }

        ck(vkDeviceWaitIdle(win.device()), "vkDeviceWaitIdle");
        for (Upload& up : uploads) {
            destroy_upload(win, up);
        }
        vkDestroyCommandPool(win.device(), command_pool, nullptr);

        av_frame_free(&frame);
        av_packet_free(&pkt);
        sws_freeContext(sws);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
