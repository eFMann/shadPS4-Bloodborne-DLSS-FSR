// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/bb_dlss5_comp.h"
#include "video_core/renderer_vulkan/vk_bb_dlss5.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

namespace {

std::mutex status_mutex;
BbDlss5Status status;

void SetStatus(const BbDlss5Status& next) {
    std::scoped_lock lock{status_mutex};
    status = next;
}

float Number(std::string_view text, float fallback) {
    float value = fallback;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && std::isfinite(value) ? value : fallback;
}

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1};

// A model starting again within this much of a change waits for the next one: a slider dragged
// would otherwise start it every frame, which the driver does not survive.
constexpr auto SettleTime = std::chrono::milliseconds{500};
constexpr auto MinRestartInterval = std::chrono::seconds{1};
constexpr auto RetryInterval = std::chrono::seconds{5};
constexpr u32 MaxFailures = 3;
// The RenoDX black level's 32x32 blocks, after the white point's 16 bytes: up to 8192x8192.
constexpr u32 MaxBlackLevelBlocks = 256 * 256;

struct Push {
    u32 mode, encoding, resample, debug_view;
    s32 scene_width, scene_height, work_width, work_height;
    float white_scale, transfer, colour, max_ratio;
    u32 blending, black_restore;
};

struct WorkImage {
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;

    WorkImage(const Instance& instance, vk::Extent2D extent)
        : image{instance.GetDevice(), instance.GetAllocator()} {
        image.Create(vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                                         .format = vk::Format::eR16G16B16A16Sfloat,
                                         .extent = {extent.width, extent.height, 1},
                                         .mipLevels = 1,
                                         .arrayLayers = 1,
                                         .samples = vk::SampleCountFlagBits::e1,
                                         .tiling = vk::ImageTiling::eOptimal,
                                         .usage = vk::ImageUsageFlagBits::eStorage |
                                                  vk::ImageUsageFlagBits::eSampled});
        auto made = instance.GetDevice().createImageViewUnique(
            vk::ImageViewCreateInfo{.image = image.image,
                                    .viewType = vk::ImageViewType::e2D,
                                    .format = vk::Format::eR16G16B16A16Sfloat,
                                    .subresourceRange = ColorRange});
        ASSERT_MSG(made.result == vk::Result::eSuccess, "DLSS 5 image view unavailable");
        view = std::move(made.value);
    }
};

} // namespace

bool ParseBbDlss5Setting(BbDlss5Settings& s, std::string_view key, std::string_view value) {
    if (!key.starts_with("dlss5"))
        return false;
    const float number = Number(value, 0.0f);
    if (key == "dlss5")
        s.enabled = number != 0.0f;
    else if (key == "dlss5_before_upscale")
        s.before_upscale = number != 0.0f;
    else if (key == "dlss5_transfer_strength")
        s.transfer_strength = std::clamp(number, 0.0f, 4.0f);
    else if (key == "dlss5_colour_strength")
        s.colour_strength = std::clamp(number, 0.0f, 4.0f);
    else if (key == "dlss5_white_point_scale")
        s.white_point_scale = std::clamp(Number(value, 4.0f), 0.05f, 64.0f);
    else if (key == "dlss5_max_ratio")
        s.max_ratio = std::clamp(Number(value, 2.0f), 1.0f, 16.0f);
    else if (key == "dlss5_working_scale")
        s.working_scale = std::clamp(Number(value, 1.0f), 0.33f, 2.0f);
    else if (key == "dlss5_downscaler")
        s.downscaler = number != 0.0f ? 1 : 0;
    else if (key == "dlss5_debug_view")
        s.debug_view = u32(std::clamp(number, 0.0f, 3.0f));
    else if (key == "dlss5_preset")
        s.preset = u32(std::clamp(number, 0.0f, 3.0f));
    else if (key == "dlss5_style")
        s.style = u32(std::clamp(number, 0.0f, 2.0f));
    else if (key == "dlss5_intensity")
        s.intensity = std::clamp(number, 0.0f, 4.0f);
    else if (key == "dlss5_local_structure")
        s.local_structure = std::clamp(number, 0.0f, 4.0f);
    else if (key == "dlss5_local_tone")
        s.local_tone = std::clamp(number, 0.0f, 4.0f);
    else if (key == "dlss5_skin_structure")
        s.skin_structure = std::clamp(Number(value, -1.0f), -1.0f, 4.0f);
    else if (key == "dlss5_auto_mask")
        s.auto_mask = number != 0.0f;
    else if (key == "dlss5_passes")
        s.passes = u32(std::clamp(number, 1.0f, 4.0f));
    else if (key == "dlss5_blending")
        s.blending = number != 0.0f ? 1 : 0;
    else if (key == "dlss5_black_restore")
        s.black_restore = u32(std::clamp(number, 0.0f, 3.0f));
    else
        return false;
    return true;
}

BbDlss5Status BbDlss5GetStatus() {
    std::scoped_lock lock{status_mutex};
    return status;
}

struct BbDlss5::Impl {
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::UniqueSampler sampler;
    std::unique_ptr<VideoCore::Buffer> white;
    std::unique_ptr<WorkImage> proxy;
    std::array<std::unique_ptr<WorkImage>, 2> answers; // passes write them in turn
    bool fresh{};

    bool started{}, ready{};
    std::string problem;
    std::optional<DlssNgx::NeuralDesc> live, wanted;
    std::chrono::steady_clock::time_point wanted_since, created_at;
    u32 failures{};
    bool reset{true};
    std::atomic<bool> evaluate_failed{false};
    u64 runs{};

    void EnsurePipeline(const Instance& instance) {
        if (pipeline)
            return;
        const auto device = instance.GetDevice();
        using T = vk::DescriptorType;
        static constexpr std::array types{T::eStorageImage, T::eStorageImage,
                                          T::eCombinedImageSampler, T::eCombinedImageSampler,
                                          T::eStorageBuffer};
        std::array<vk::DescriptorSetLayoutBinding, types.size()> bindings;
        for (u32 i = 0; i < types.size(); ++i)
            bindings[i] = {.binding = i,
                           .descriptorType = types[i],
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        descriptors = device
                          .createDescriptorSetLayoutUnique(
                              {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
                               .bindingCount = u32(bindings.size()),
                               .pBindings = bindings.data()})
                          .value;
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .size = sizeof(Push)};
        layout = device
                     .createPipelineLayoutUnique({.setLayoutCount = 1,
                                                  .pSetLayouts = &descriptors.get(),
                                                  .pushConstantRangeCount = 1,
                                                  .pPushConstantRanges = &range})
                     .value;
        const auto module = CompileSPV(BB_DLSS5_COMP, device);
        ASSERT_MSG(module, "DLSS 5 shader unavailable");
        pipeline = device
                       .createComputePipelineUnique(
                           {},
                           vk::ComputePipelineCreateInfo{
                               .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                         .module = module,
                                         .pName = "main"},
                               .layout = *layout})
                       .value;
        device.destroyShaderModule(module);
        sampler = device
                      .createSamplerUnique({.magFilter = vk::Filter::eNearest,
                                            .minFilter = vk::Filter::eNearest,
                                            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                            .maxLod = 0.0f})
                      .value;
        white = std::make_unique<VideoCore::Buffer>(instance, 0, 16 + 16 * MaxBlackLevelBlocks,
                                                    VideoCore::MemoryType::DeviceLocal);
    }

    // After the GPU was drained.
    void Release(DlssNgx* ngx) {
        if (live && ngx)
            ngx->NeuralReleaseAfterGpuDrain();
        live.reset();
    }

    void Stop(Scheduler& scheduler, DlssNgx* ngx) {
        if (!live)
            return;
        scheduler.EndRendering();
        scheduler.Finish();
        Release(ngx);
    }

    // Whether the model can run this frame; starts it, or again with changed settings.
    bool Prepare(const Instance& instance, Scheduler& scheduler, DlssNgx* ngx,
                 const DlssNgx::NeuralDesc& desc) {
        if (evaluate_failed.exchange(false)) {
            Stop(scheduler, ngx);
            ready = false;
            problem = "the model failed while running and was turned off until the next start";
            LOG_ERROR(Render_Vulkan, "[DLSS 5] Evaluation failed; turned off");
        }
        if (!started) {
            started = true;
            ready = ngx->NeuralInitialize(problem);
            LOG_INFO(Render_Vulkan, "[DLSS 5] {}", ready ? "Ready" : problem);
        }
        if (!ready)
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (!wanted || *wanted != desc) {
            wanted = desc;
            wanted_since = now;
        }
        const bool due = !live ? failures == 0 || now - created_at >= RetryInterval
                               : *live != desc && now - wanted_since >= SettleTime &&
                                     now - created_at >= MinRestartInterval;
        if (due) {
            scheduler.EndRendering();
            scheduler.Finish();
            Release(ngx);
            const vk::Extent2D extent{desc.width, desc.height};
            if (!proxy || proxy->image.image_ci.extent.width != extent.width ||
                proxy->image.image_ci.extent.height != extent.height) {
                proxy = std::make_unique<WorkImage>(instance, extent);
                for (auto& answer : answers)
                    answer = std::make_unique<WorkImage>(instance, extent);
                fresh = true;
            }
            created_at = now;
            if (!ngx->NeuralCreate(scheduler.CommandBuffer(), desc)) {
                problem =
                    fmt::format("the model would not start at {}x{}", desc.width, desc.height);
                LOG_WARNING(Render_Vulkan, "[DLSS 5] {}", problem);
                if (++failures >= MaxFailures)
                    ready = false;
                return false;
            }
            scheduler.Finish(); // the model's start-up work runs before its first evaluation
            failures = 0;
            live = desc;
            reset = true;
            problem.clear();
            LOG_INFO(Render_Vulkan,
                     "[DLSS 5] Model {}x{} preset {} style {} intensity {} local structure {} "
                     "local tone {} skin structure {} auto mask {} passes {}",
                     desc.width, desc.height, desc.preset, desc.style, desc.intensity,
                     desc.local_structure, desc.local_tone, desc.skin_structure, desc.auto_mask,
                     desc.passes);
        }
        return live.has_value();
    }
};

BbDlss5::BbDlss5() : impl{std::make_unique<Impl>()} {}
BbDlss5::~BbDlss5() = default;

bool BbDlss5::Apply(const Instance& instance, Scheduler& scheduler, const BbDlss5Settings& settings,
                    const Inputs& in) {
    auto& s = *impl;
    auto* ngx = instance.GetDlssNgx();
    BbDlss5Status next{.enabled = settings.enabled,
                       .hdr = in.encoding == Encoding::Hdr,
                       .width = in.extent.width,
                       .height = in.extent.height};
    if (!settings.enabled || !ngx) {
        s.Stop(scheduler, ngx);
        if (settings.enabled)
            next.problem = "DLSS is not running on this GPU";
        SetStatus(next);
        return false;
    }
    const float scale = std::clamp(settings.working_scale, 0.5f, 2.0f);
    const DlssNgx::NeuralDesc desc{
        .width = std::max(64u, u32(std::lround(float(in.extent.width) * scale))),
        .height = std::max(64u, u32(std::lround(float(in.extent.height) * scale))),
        .preset = settings.preset,
        .style = settings.style,
        .intensity = settings.intensity,
        .local_structure = settings.local_structure,
        .local_tone = settings.local_tone,
        .skin_structure =
            settings.skin_structure < 0.0f ? settings.local_structure : settings.skin_structure,
        .auto_mask = settings.auto_mask,
        .passes = std::clamp(settings.passes, 1u, 4u)};
    if (!s.Prepare(instance, scheduler, ngx, desc)) {
        next.problem = s.problem.empty() ? "Starting..." : s.problem;
        SetStatus(next);
        return false;
    }
    s.EnsurePipeline(instance);
    const vk::Extent2D work{s.live->width, s.live->height};
    next.running = true;
    next.model_width = work.width;
    next.model_height = work.height;
    SetStatus(next);

    auto* profiler = GpuProfiler::Get();
    if (profiler && profiler->Records(&scheduler))
        profiler->Mark(0xD155'5ull, [] { return std::string{"DLSS 5"}; });
    else
        profiler = nullptr;

    Push push{.encoding = u32(in.encoding),
              .resample = settings.downscaler,
              .debug_view = settings.debug_view,
              .scene_width = s32(in.extent.width),
              .scene_height = s32(in.extent.height),
              .work_width = s32(work.width),
              .work_height = s32(work.height),
              .white_scale = settings.white_point_scale,
              .transfer = settings.transfer_strength,
              .colour = settings.colour_strength,
              .max_ratio = settings.max_ratio,
              .blending = settings.blending};
    const u32 blocks_x = (in.extent.width + 31) / 32;
    const u32 blocks_y = (in.extent.height + 31) / 32;
    const bool black_level = settings.blending == 1 && settings.black_restore != 0 &&
                             settings.debug_view == 0 && blocks_x * blocks_y <= MaxBlackLevelBlocks;
    if (black_level)
        push.black_restore = settings.black_restore;
    const bool fresh = std::exchange(s.fresh, false);
    const bool reset = std::exchange(s.reset, false) || in.reset;
    const u32 passes = s.live->passes;
    const std::array answer_images{s.answers[0]->image.image, s.answers[1]->image.image};
    const std::array answer_views{*s.answers[0]->view, *s.answers[1]->view};
    scheduler.Record([pipeline = *s.pipeline, layout = *s.layout, sampler = *s.sampler,
                      white = s.white->Handle(), proxy_image = s.proxy->image.image,
                      proxy_view = *s.proxy->view, answer_images, answer_views, passes, in, work,
                      push, fresh, reset, ngx, black_level, blocks_x, blocks_y,
                      failed = &s.evaluate_failed](vk::CommandBuffer command) {
        const u32 last = (passes - 1) % 2;
        const auto all = [&](vk::PipelineStageFlags2 from, vk::PipelineStageFlags2 to) {
            const vk::MemoryBarrier2 barrier{.srcStageMask = from,
                                             .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                             .dstStageMask = to,
                                             .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                              vk::AccessFlagBits2::eMemoryWrite};
            command.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        };
        using S = vk::PipelineStageFlagBits2;
        if (fresh) {
            std::array<vk::ImageMemoryBarrier2, 3> images;
            for (u32 i = 0; i < 3; ++i)
                images[i] = {.srcStageMask = S::eNone,
                             .dstStageMask = S::eAllCommands,
                             .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                              vk::AccessFlagBits2::eMemoryWrite,
                             .oldLayout = vk::ImageLayout::eUndefined,
                             .newLayout = vk::ImageLayout::eGeneral,
                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .image = i == 0 ? proxy_image : answer_images[i - 1],
                             .subresourceRange = ColorRange};
            command.pipelineBarrier2({.imageMemoryBarrierCount = u32(images.size()),
                                      .pImageMemoryBarriers = images.data()});
            command.fillBuffer(white, 0, VK_WHOLE_SIZE, 0);
        }
        all(S::eAllCommands, S::eComputeShader);
        const std::array image_infos{
            vk::DescriptorImageInfo{{}, in.view, vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{{}, proxy_view, vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{sampler, proxy_view, vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{sampler, answer_views[last], vk::ImageLayout::eGeneral}};
        const vk::DescriptorBufferInfo buffer_info{white, 0, VK_WHOLE_SIZE};
        std::array<vk::WriteDescriptorSet, 5> writes;
        for (u32 i = 0; i < 4; ++i)
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i < 2 ? vk::DescriptorType::eStorageImage
                                                 : vk::DescriptorType::eCombinedImageSampler,
                         .pImageInfo = &image_infos[i]};
        writes[4] = {.dstBinding = 4,
                     .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                     .pBufferInfo = &buffer_info};
        const auto bind = [&] {
            command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        };
        const auto run = [&](u32 mode, u32 groups_x, u32 groups_y) {
            Push p = push;
            p.mode = mode;
            command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(p), &p);
            command.dispatch(groups_x, groups_y, 1);
        };
        bind();
        if (in.encoding == Encoding::Hdr) {
            run(0, 1, 1);
            all(S::eComputeShader, S::eComputeShader);
        }
        run(1, (work.width + 15) / 16, (work.height + 15) / 16);
        all(S::eComputeShader, S::eAllCommands);
        const auto resource = [&](vk::Image image, vk::ImageView view) {
            return DlssNgx::Resource{image, view, ColorRange, vk::Format::eR16G16B16A16Sfloat,
                                     work};
        };
        // Each pass is shown the one before's answer.
        for (u32 pass = 0; pass < passes; ++pass) {
            const auto input =
                pass == 0 ? resource(proxy_image, proxy_view)
                          : resource(answer_images[(pass - 1) % 2], answer_views[(pass - 1) % 2]);
            const auto output = resource(answer_images[pass % 2], answer_views[pass % 2]);
            if (pass > 0)
                all(S::eAllCommands, S::eAllCommands);
            if (!ngx->NeuralEvaluate(command, input, in.depth, in.motion, output, in.depth_inverted,
                                     reset, pass))
                failed->store(true);
        }
        all(S::eAllCommands, S::eComputeShader);
        bind();
        // RenoDX's black level is measured on the scene before the answer is written into it.
        if (black_level) {
            run(3, blocks_x, blocks_y);
            all(S::eComputeShader, S::eComputeShader);
        }
        run(2, (in.extent.width + 15) / 16, (in.extent.height + 15) / 16);
        if (black_level) {
            all(S::eComputeShader, S::eComputeShader);
            run(4, (in.extent.width + 15) / 16, (in.extent.height + 15) / 16);
        }
        all(S::eComputeShader, S::eAllCommands);
    });
    if (profiler)
        profiler->Mark(0xD155ull, [] { return std::string{"DLSS (temporal upscale and passes)"}; });
    if (++s.runs <= 3 || s.runs % 3600 == 0)
        LOG_INFO(Render_Vulkan,
                 "[DLSS 5] runs={} scene {}x{} model {}x{} passes {} encoding {} debug {} "
                 "blending {} black level {}",
                 s.runs, in.extent.width, in.extent.height, work.width, work.height, passes,
                 u32(in.encoding), settings.debug_view, settings.blending,
                 black_level ? settings.black_restore : 0);
    return true;
}

void BbDlss5::Shutdown(DlssNgx* ngx) {
    impl->Release(ngx);
    impl->proxy.reset();
    for (auto& answer : impl->answers)
        answer.reset();
    impl->white.reset();
}

} // namespace Vulkan
