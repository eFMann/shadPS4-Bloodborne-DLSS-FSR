// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/bb_ssgi_blur_comp.h"
#include "video_core/host_shaders/bb_ssgi_capture_comp.h"
#include "video_core/host_shaders/bb_ssgi_composite_comp.h"
#include "video_core/host_shaders/bb_ssgi_show_bounce_comp.h"
#include "video_core/host_shaders/bb_ssgi_temporal_comp.h"
#include "video_core/host_shaders/bb_ssgi_trace_comp.h"
#include "video_core/renderer_vulkan/vk_bb_ssgi.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

template <typename T>
T CheckOrThrow(vk::ResultValue<T> result) {
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error(vk::to_string(result.result));
    }
    return std::move(result.value);
}

/// As bb_ssgi.comp's push constants (128 bytes, the guaranteed minimum).
struct PushConstants {
    std::array<float, 4> proj{};      // x scale, y scale, z scale, z offset (ndc +y down)
    std::array<float, 2> prev_proj{}; // the reprojection's camera: x scale, y scale
    std::array<float, 2> size{};      // scene pixels
    std::array<float, 9> view3{};     // world to view rotation, rows
    std::array<float, 12> reproj{};   // a past camera's view * inverse current view (3x4)
    float param0{};
    float param1{};
    u32 flags{};
};
static_assert(sizeof(PushConstants) == 128);

constexpr u32 FlagLight = 1u << 31;
constexpr u32 FlagNormals = 1u << 30;
constexpr u32 FlagAo = 1u << 30;
constexpr u32 FlagSubtract = 1u << 28;
constexpr u32 FlagSelfLit = 1u << 27;

constexpr float Thickness = 0.5f;     // view units (metres) behind a depth sample
constexpr float TemporalBlend = 0.9f; // about 10 frames of history
// The game's surfaces are dark (base colours of about 0.1 to 0.3) and its light is tone mapped
// low: a single physically scaled bounce adds a few percent, too little to see. Strength 1 is
// this much.
constexpr float BounceGain = 8.0f;

// 3x4 affine matrices (rows; the last column is the translation), as for the better AO.
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

/// The inverse of [M | t]: [M^-1 | -M^-1 t].
std::array<float, 12> InverseAffine(const std::array<float, 12>& m) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12f) {
        return m;
    }
    const float s = 1.0f / det;
    const std::array<float, 9> r = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s,
                                    (f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s,
                                    (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s};
    std::array<float, 12> out{};
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = r[row * 3 + 0];
        out[row * 4 + 1] = r[row * 3 + 1];
        out[row * 4 + 2] = r[row * 3 + 2];
        out[row * 4 + 3] =
            -(r[row * 3 + 0] * m[3] + r[row * 3 + 1] * m[7] + r[row * 3 + 2] * m[11]);
    }
    return out;
}

vk::ImageMemoryBarrier2 ComputeBarrier(vk::Image image, vk::ImageLayout old_layout,
                                       vk::AccessFlags2 src, vk::AccessFlags2 dst) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = src,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = dst,
        .oldLayout = old_layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
}

constexpr auto ReadWrite = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;

} // Anonymous namespace

struct BbSsgi::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::TextureCache& texture_cache;
    vk::Device device;
    bool failed{};

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline capture_pipeline;
    vk::UniquePipeline trace_pipeline;
    vk::UniquePipeline temporal_pipeline;
    vk::UniquePipeline blur_pipeline;
    vk::UniquePipeline composite_pipeline;
    vk::UniquePipeline show_bounce_pipeline;

    /// RGBA16F at half the scene's size: light (or indirect light) and view z.
    struct Scratch {
        std::unique_ptr<VideoCore::UniqueImage> image;
        vk::UniqueImageView view;
    };
    Scratch light;      ///< the surfaces' diffuse light (CAPTURE), for the next frame
    Scratch raw;        ///< this frame's bounce (TRACE)
    Scratch history[2]; ///< accumulated bounce: this frame's and the last one's, by turns
    Scratch blurred;    ///< the accumulated bounce, blurred (BLUR)
    Scratch ambient;    ///< the game's ambient diffuse light, at the lighting pass (CAPTURE)
    u32 history_index{};
    bool scratch_fresh{}; ///< the scratch images are still in Undefined layout
    std::array<u32, 2> scratch_size{};
    u32 frame{};

    // The scene: the largest G-buffer pass of the frame, its normals and its camera.
    VideoCore::ImageId depth_id{};
    VideoCore::ImageId normals_id{};
    VideoCore::ImageId ao_id{}; ///< this frame's AO at the scene size
    u64 depth_area{};
    bool side_pass{};
    bool pass_has_camera{};
    bool lighting_done{}; ///< the frame's lighting ran: the next G-buffer pass is a new frame
    float x_sign{1.0f}, y_sign{1.0f};
    std::array<float, 12> view{};
    std::array<float, 4> proj{};
    std::array<u32, 2> render_size{};

    // Frames are counted at the lighting passes: the light kept and the history are used only
    // by the frame right after theirs.
    u64 lighting_index{};
    u64 light_frame{~0ull};
    std::array<float, 12> light_view{};
    std::array<float, 4> light_proj{};
    u64 history_frame{~0ull};
    std::array<float, 12> history_view{};
    std::array<float, 4> history_proj{};
    // The debug view: the frame whose bounce is in `blurred`, and how it was added.
    u64 bounce_frame{~0ull};
    // The ambient light kept at this frame's lighting pass, and the image it was in.
    u64 ambient_frame{~0ull};
    VideoCore::ImageId diffuse_light{};
    /// This frame's diffuse light times the base colour (8650418f's target), kept at the
    /// scene's composite.
    VideoCore::ImageId diffuse_albedo{};
    PushConstants last_composite{};
    VideoCore::ImageId bounce_ao{};

    struct Stats {
        u64 lighting{}, ran{}, with_light{}, with_history{}, captured{};
        u64 no_camera{}, no_depth{}, not_fitting{}, capture_skipped{}, without_ao{};
        u64 without_ambient{}, with_self_lit{};
    } stats;
    std::chrono::steady_clock::time_point next_log{};

    Impl(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
         VideoCore::TextureCache& texture_cache_)
        : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
          texture_cache{texture_cache_}, device{instance_.GetDevice()} {}

    void LogStats() {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_log) {
            return;
        }
        if (next_log != std::chrono::steady_clock::time_point{} && stats.lighting) {
            LOG_INFO(Render_Vulkan,
                     "SSGI (5 s): lighting passes {}, bounce added {} (with last frame's light "
                     "{}, with history {}, without the game's AO {}); light kept {} (with the "
                     "ambient light in it {}, with the self-lit layer {}), not kept {}; skipped: "
                     "no camera {}, no depth {}, targets not fitting {}",
                     stats.lighting, stats.ran, stats.with_light, stats.with_history,
                     stats.without_ao, stats.captured, stats.without_ambient, stats.with_self_lit,
                     stats.capture_skipped, stats.no_camera, stats.no_depth, stats.not_fitting);
        }
        stats = {};
        next_log = now + std::chrono::seconds(5);
    }

    void CreatePipelines() {
        const std::array<vk::DescriptorSetLayoutBinding, 6> bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 3,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 4,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 5,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        desc_layout = CheckOrThrow(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(PushConstants)};
        const auto set = *desc_layout;
        pipeline_layout = CheckOrThrow(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &set,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto create = [&](std::span<const u32> code) {
            const auto module = CompileSPV(code, device);
            auto result = device.createComputePipelineUnique(
                {}, vk::ComputePipelineCreateInfo{
                        .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                  .module = module,
                                  .pName = "main"},
                        .layout = *pipeline_layout,
                    });
            device.destroyShaderModule(module);
            return CheckOrThrow(std::move(result));
        };
        capture_pipeline = create(BB_SSGI_CAPTURE_COMP);
        trace_pipeline = create(BB_SSGI_TRACE_COMP);
        temporal_pipeline = create(BB_SSGI_TEMPORAL_COMP);
        blur_pipeline = create(BB_SSGI_BLUR_COMP);
        composite_pipeline = create(BB_SSGI_COMPOSITE_COMP);
        show_bounce_pipeline = create(BB_SSGI_SHOW_BOUNCE_COMP);
    }

    Scratch MakeScratch(u32 width, u32 height) {
        Scratch out;
        out.image = std::make_unique<VideoCore::UniqueImage>(device, instance.GetAllocator());
        out.image->Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        out.view = CheckOrThrow(device.createImageViewUnique({
            .image = out.image->image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        return out;
    }

    /// The scratch images for a scene of `size` pixels (half of it, rounded up). Made again
    /// when the scene size changes, without the light and history kept.
    void EnsureScratch(std::array<u32, 2> size) {
        if (light.image && scratch_size == size) {
            return;
        }
        if (light.image) {
            // The old ones may still be in use by the GPU.
            scheduler.DeferOperation([old_light = std::move(light), old_raw = std::move(raw),
                                      old0 = std::move(history[0]), old1 = std::move(history[1]),
                                      old_blurred = std::move(blurred),
                                      old_ambient = std::move(ambient)] {});
        }
        const u32 width = (size[0] + 1) / 2;
        const u32 height = (size[1] + 1) / 2;
        light = MakeScratch(width, height);
        raw = MakeScratch(width, height);
        history[0] = MakeScratch(width, height);
        history[1] = MakeScratch(width, height);
        blurred = MakeScratch(width, height);
        ambient = MakeScratch(width, height);
        scratch_size = size;
        scratch_fresh = true;
        light_frame = ~0ull;
        history_frame = ~0ull;
    }

    /// The barriers that take fresh scratch images out of Undefined layout, once.
    std::vector<vk::ImageMemoryBarrier2> TakeFreshBarriers() {
        std::vector<vk::ImageMemoryBarrier2> out;
        if (!scratch_fresh) {
            return out;
        }
        scratch_fresh = false;
        for (const Scratch* s : {&light, &raw, &history[0], &history[1], &blurred, &ambient}) {
            out.push_back(
                ComputeBarrier(s->image->image, vk::ImageLayout::eUndefined, {}, ReadWrite));
        }
        return out;
    }

    /// A view the texture cache keeps with the image (made once, dropped with the image).
    vk::ImageView CacheView(VideoCore::Image& image, vk::Format format, bool storage) {
        VideoCore::ImageViewInfo info{};
        info.type = AmdGpu::ImageType::Color2D;
        info.format = format;
        info.is_storage = storage;
        return *image.FindView(info, false).image_view;
    }

    /// The scene's size in pixels: the viewport of its camera, within the depth image (which
    /// may be allocated larger).
    std::array<u32, 2> SceneSize(const VideoCore::Image& depth) const {
        const u32 width = render_size[0] ? std::min(render_size[0], depth.info.size.width)
                                         : depth.info.size.width;
        const u32 height = render_size[1] ? std::min(render_size[1], depth.info.size.height)
                                          : depth.info.size.height;
        return {width, height};
    }

    bool IsSceneDepth(const VideoCore::Image& depth) const {
        const auto format = depth.info.pixel_format;
        return (format == vk::Format::eD32Sfloat || format == vk::Format::eD32SfloatS8Uint) &&
               depth.info.num_samples == 1;
    }

    /// An image of the scene's size that can be read.
    bool SceneSized(const VideoCore::Image& image, const VideoCore::Image& depth) const {
        return !image.info.props.is_depth && image.backing && image.info.num_samples == 1 &&
               image.info.size.width == depth.info.size.width &&
               image.info.size.height == depth.info.size.height;
    }

    void FillCamera(PushConstants& push, std::array<u32, 2> size) const {
        push.proj = proj;
        push.size = {float(size[0]), float(size[1])};
        push.view3 = {view[0], view[1], view[2], view[4], view[5],
                      view[6], view[8], view[9], view[10]};
    }

    void Reprojection(PushConstants& push, const std::array<float, 12>& past_view,
                      const std::array<float, 4>& past_proj) const {
        push.reproj = Multiply(past_view, InverseAffine(view));
        push.prev_proj = {past_proj[0], past_proj[1]};
    }

    static std::array<u32, 2> Groups(std::array<float, 2> size, bool half) {
        const u32 width = half ? (u32(size[0]) + 1) / 2 : u32(size[0]);
        const u32 height = half ? (u32(size[1]) + 1) / 2 : u32(size[1]);
        return {(width + 7) / 8, (height + 7) / 8};
    }

    /// Bindings 0 to 2, 4 and 5 sampled (`sampled`, in that order; those left out get the
    /// first), 3 the storage image.
    static void PushImages(vk::CommandBuffer cmdbuf, vk::PipelineLayout layout,
                           std::initializer_list<vk::ImageView> sampled_list,
                           vk::ImageView storage) {
        std::array<vk::ImageView, 5> sampled{};
        std::ranges::copy(sampled_list, sampled.begin());
        for (auto& view : sampled) {
            view = view ? view : *sampled_list.begin();
        }
        std::array<vk::DescriptorImageInfo, 6> infos{};
        std::array<vk::WriteDescriptorSet, 6> writes{};
        for (u32 i = 0; i < 6; ++i) {
            infos[i] = {.imageView = i == 3 ? storage : sampled[i < 3 ? i : i - 1],
                        .imageLayout = vk::ImageLayout::eGeneral};
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i == 3 ? vk::DescriptorType::eStorageImage
                                                  : vk::DescriptorType::eSampledImage,
                         .pImageInfo = &infos[i]};
        }
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
    }

    void Run(VideoCore::ImageId diffuse_id, vk::Format view_format) {
        auto& depth = texture_cache.GetImage(depth_id);
        auto& diffuse = texture_cache.GetImage(diffuse_id);
        const auto diffuse_format =
            view_format == vk::Format::eUndefined ? diffuse.info.pixel_format : view_format;
        const bool fits = IsSceneDepth(depth) && SceneSized(diffuse, depth) &&
                          diffuse_format == vk::Format::eB10G11R11UfloatPack32 &&
                          diffuse.info.pixel_format == vk::Format::eB10G11R11UfloatPack32 &&
                          bool(diffuse.usage_flags & vk::ImageUsageFlagBits::eStorage);
        if (!fits) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_WARNING(Render_Vulkan,
                            "SSGI: skipped (depth {} {}x{}, diffuse light {} {}x{} drawn as {})",
                            vk::to_string(depth.info.pixel_format), depth.info.size.width,
                            depth.info.size.height, vk::to_string(diffuse.info.pixel_format),
                            diffuse.info.size.width, diffuse.info.size.height,
                            vk::to_string(view_format));
            }
            ++stats.not_fitting;
            return;
        }
        VideoCore::Image* normals = nullptr;
        if (normals_id && normals_id != diffuse_id) {
            auto& image = texture_cache.GetImage(normals_id);
            if (SceneSized(image, depth) && image.info.pixel_format == vk::Format::eR8G8B8A8Unorm) {
                normals = &image;
            }
        }
        VideoCore::Image* ao = nullptr;
        if (ao_id && ao_id != diffuse_id) {
            auto& image = texture_cache.GetImage(ao_id);
            if (SceneSized(image, depth)) {
                ao = &image;
            }
        }
        if (!trace_pipeline) {
            CreatePipelines();
        }
        const auto size = SceneSize(depth);
        {
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG_INFO(Render_Vulkan,
                         "SSGI: on, bounce traced at {}x{} into the diffuse light ({}x{}); "
                         "G-buffer normals {}, the game's AO {}",
                         (size[0] + 1) / 2, (size[1] + 1) / 2, diffuse.info.size.width,
                         diffuse.info.size.height,
                         normals ? vk::to_string(normals->info.pixel_format) : "none",
                         ao ? vk::to_string(ao->info.pixel_format) : "none");
            }
        }
        EnsureScratch(size);

        const u64 now = lighting_index;
        const bool has_light = light_frame + 1 == now;
        const bool has_history = history_frame + 1 == now;
        const float strength =
            std::clamp(float(EmulatorSettings.GetBbSsgiStrength()) * 0.01f, 0.0f, 4.0f) *
            BounceGain;
        const float reach =
            std::clamp(float(EmulatorSettings.GetBbSsgiRadius()) * 0.01f, 0.25f, 20.0f);

        PushConstants trace{};
        FillCamera(trace, size);
        if (has_light) {
            Reprojection(trace, light_view, light_proj);
        }
        trace.param0 = reach;
        trace.param1 = Thickness;
        trace.flags =
            (++frame & 0xffffu) | (has_light ? FlagLight : 0u) | (normals ? FlagNormals : 0u);

        PushConstants temporal{};
        FillCamera(temporal, size);
        if (has_history) {
            Reprojection(temporal, history_view, history_proj);
        }
        temporal.param0 = has_history ? TemporalBlend : 0.0f;

        PushConstants blur{};
        FillCamera(blur, size);

        PushConstants composite{};
        FillCamera(composite, size);
        composite.param0 = strength;
        composite.flags = ao ? FlagAo : 0u;
        last_composite = composite;
        bounce_frame = now;
        bounce_ao = ao_id;

        const u32 write_index = history_index ^ 1u;
        history_index = write_index;
        history_frame = now;
        history_view = view;
        history_proj = proj;
        ++stats.ran;
        stats.with_light += has_light ? 1 : 0;
        stats.with_history += has_history ? 1 : 0;
        stats.without_ao += ao ? 0 : 1;

        scheduler.EndRendering();
        runtime.Transit(&depth, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        if (normals) {
            runtime.Transit(normals, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderRead);
        }
        if (ao) {
            runtime.Transit(ao, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderRead);
        }
        runtime.Transit(&diffuse, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader, ReadWrite);
        runtime.FlushBarriers();

        // R32 float: a depth-only view of the depth image (ImageView picks the depth aspect).
        const auto depth_view = CacheView(depth, vk::Format::eR32Sfloat, false);
        // Bindings left unread get the depth.
        const auto normal_view =
            normals ? CacheView(*normals, normals->info.pixel_format, false) : depth_view;
        const auto ao_view = ao ? CacheView(*ao, ao->info.pixel_format, false) : depth_view;
        const auto diffuse_view = CacheView(diffuse, vk::Format::eB10G11R11UfloatPack32, true);
        const auto diffuse_read = CacheView(diffuse, vk::Format::eB10G11R11UfloatPack32, false);
        PushConstants keep_ambient{};
        FillCamera(keep_ambient, size);
        ambient_frame = now;
        diffuse_light = diffuse_id;

        scheduler.Record([trace, temporal, blur, composite, keep_ambient, depth_view, normal_view,
                          ao_view, diffuse_view, diffuse_read, ambient = ambient.image->image,
                          ambient_view = *ambient.view, capture_pipeline = *capture_pipeline,
                          fresh = TakeFreshBarriers(), light = light.image->image,
                          light_view = *light.view, raw = raw.image->image, raw_view = *raw.view,
                          hist_write = history[write_index].image->image,
                          hist_write_view = *history[write_index].view,
                          hist_read_view = *history[write_index ^ 1u].view,
                          blurred = blurred.image->image, blurred_view = *blurred.view,
                          trace_pipeline = *trace_pipeline, temporal_pipeline = *temporal_pipeline,
                          blur_pipeline = *blur_pipeline, composite_pipeline = *composite_pipeline,
                          layout = *pipeline_layout](vk::CommandBuffer cmdbuf) {
            const auto barrier = [&](std::span<const vk::ImageMemoryBarrier2> barriers) {
                cmdbuf.pipelineBarrier2(
                    vk::DependencyInfo{.imageMemoryBarrierCount = u32(barriers.size()),
                                       .pImageMemoryBarriers = barriers.data()});
            };
            const auto dispatch = [&](vk::Pipeline pipeline, const PushConstants& push,
                                      std::initializer_list<vk::ImageView> sampled,
                                      vk::ImageView storage, bool half) {
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
                PushImages(cmdbuf, layout, sampled, storage);
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                                     &push);
                const auto groups = Groups(push.size, half);
                cmdbuf.dispatch(groups[0], groups[1], 1);
            };
            if (!fresh.empty()) {
                barrier(fresh);
            }
            // The scratch images are reused each frame: last frame's reads before these writes,
            // and the light kept last frame written before the trace reads it.
            const std::array before = {
                ComputeBarrier(light, vk::ImageLayout::eGeneral, ReadWrite, ReadWrite),
                ComputeBarrier(raw, vk::ImageLayout::eGeneral, ReadWrite, ReadWrite),
                ComputeBarrier(hist_write, vk::ImageLayout::eGeneral, ReadWrite, ReadWrite),
                ComputeBarrier(blurred, vk::ImageLayout::eGeneral, ReadWrite, ReadWrite),
                ComputeBarrier(ambient, vk::ImageLayout::eGeneral, ReadWrite, ReadWrite),
            };
            barrier(before);
            // The game's own diffuse light, before the bounce goes into it.
            dispatch(capture_pipeline, keep_ambient, {diffuse_read, depth_view}, ambient_view,
                     true);
            dispatch(trace_pipeline, trace, {depth_view, normal_view, light_view}, raw_view, true);
            const std::array traced = {ComputeBarrier(raw, vk::ImageLayout::eGeneral,
                                                      vk::AccessFlagBits2::eShaderWrite,
                                                      vk::AccessFlagBits2::eShaderRead)};
            barrier(traced);
            dispatch(temporal_pipeline, temporal, {raw_view, depth_view, hist_read_view},
                     hist_write_view, true);
            const std::array accumulated = {ComputeBarrier(hist_write, vk::ImageLayout::eGeneral,
                                                           vk::AccessFlagBits2::eShaderWrite,
                                                           vk::AccessFlagBits2::eShaderRead)};
            barrier(accumulated);
            dispatch(blur_pipeline, blur, {hist_write_view, depth_view, depth_view}, blurred_view,
                     true);
            const std::array blurred_done = {ComputeBarrier(blurred, vk::ImageLayout::eGeneral,
                                                            vk::AccessFlagBits2::eShaderWrite,
                                                            vk::AccessFlagBits2::eShaderRead)};
            barrier(blurred_done);
            // The ambient pass read the diffuse light the composite now writes.
            const vk::MemoryBarrier2 read_before_write{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            };
            cmdbuf.pipelineBarrier2(
                vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &read_before_write});
            dispatch(composite_pipeline, composite, {depth_view, blurred_view, ao_view},
                     diffuse_view, false);
        });
    }

    void Capture(VideoCore::ImageId target_id, VideoCore::ImageId self_lit_id) {
        auto& depth = texture_cache.GetImage(depth_id);
        auto& target = texture_cache.GetImage(target_id);
        if (!IsSceneDepth(depth) || !SceneSized(target, depth) ||
            target.info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
            ++stats.capture_skipped;
            return;
        }
        const auto size = SceneSize(depth);
        if (!capture_pipeline) {
            CreatePipelines();
        }
        EnsureScratch(size);
        PushConstants push{};
        FillCamera(push, size);
        // The diffuse light it multiplied by the base colour, and its ambient part.
        VideoCore::Image* total = nullptr;
        if (ambient_frame == lighting_index && diffuse_light && diffuse_light != target_id) {
            auto& image = texture_cache.GetImage(diffuse_light);
            if (SceneSized(image, depth) &&
                image.info.pixel_format == vk::Format::eB10G11R11UfloatPack32) {
                total = &image;
                push.flags = FlagSubtract;
            }
        }
        stats.without_ambient += total ? 0 : 1;
        VideoCore::Image* self_lit = nullptr;
        if (self_lit_id && self_lit_id != target_id) {
            auto& image = texture_cache.GetImage(self_lit_id);
            const auto format = image.info.pixel_format;
            const bool usable =
                SceneSized(image, depth) && (format == vk::Format::eR16G16B16A16Sfloat ||
                                             format == vk::Format::eB10G11R11UfloatPack32);
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG_INFO(Render_Vulkan,
                         "SSGI: self-lit layer (the scene composite's texture 5): {} {}x{}{}",
                         vk::to_string(format), image.info.size.width, image.info.size.height,
                         usable ? "" : ", not used");
            }
            if (usable) {
                self_lit = &image;
                push.flags |= FlagSelfLit;
            }
        }
        stats.with_self_lit += self_lit ? 1 : 0;
        light_frame = lighting_index;
        light_view = view;
        light_proj = proj;
        ++stats.captured;

        scheduler.EndRendering();
        runtime.Transit(&depth, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&target, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        if (total) {
            runtime.Transit(total, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderRead);
        }
        if (self_lit) {
            runtime.Transit(self_lit, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderRead);
        }
        runtime.FlushBarriers();
        const auto depth_view = CacheView(depth, vk::Format::eR32Sfloat, false);
        const auto target_view = CacheView(target, vk::Format::eR16G16B16A16Sfloat, false);
        const auto total_view =
            total ? CacheView(*total, vk::Format::eB10G11R11UfloatPack32, false) : depth_view;
        const auto self_lit_view =
            self_lit ? CacheView(*self_lit, self_lit->info.pixel_format, false) : depth_view;
        scheduler.Record([push, depth_view, target_view, total_view, self_lit_view,
                          ambient_view = *ambient.view, fresh = TakeFreshBarriers(),
                          light = light.image->image, light_view = *light.view,
                          pipeline = *capture_pipeline,
                          layout = *pipeline_layout](vk::CommandBuffer cmdbuf) {
            std::vector<vk::ImageMemoryBarrier2> barriers = fresh;
            // This frame's trace read the light kept last frame.
            barriers.push_back(
                ComputeBarrier(light, vk::ImageLayout::eGeneral, ReadWrite, ReadWrite));
            cmdbuf.pipelineBarrier2(
                vk::DependencyInfo{.imageMemoryBarrierCount = u32(barriers.size()),
                                   .pImageMemoryBarriers = barriers.data()});
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            PushImages(cmdbuf, layout,
                       {target_view, depth_view, total_view, ambient_view, self_lit_view},
                       light_view);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            const auto groups = Groups(push.size, true);
            cmdbuf.dispatch(groups[0], groups[1], 1);
        });
    }

    /// The debug views: 1, the bounce; 2, the light that bounces (kept this frame).
    void ShowBounce(VideoCore::ImageId target_id, int mode) {
        auto& depth = texture_cache.GetImage(depth_id);
        auto& target = texture_cache.GetImage(target_id);
        if (!IsSceneDepth(depth) || !SceneSized(target, depth) ||
            target.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
            !(target.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            return;
        }
        VideoCore::Image* ao = nullptr;
        if (bounce_ao && bounce_ao != target_id) {
            auto& image = texture_cache.GetImage(bounce_ao);
            if (SceneSized(image, depth)) {
                ao = &image;
            }
        }
        scheduler.EndRendering();
        runtime.Transit(&depth, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        if (ao) {
            runtime.Transit(ao, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderRead);
        }
        runtime.Transit(&target, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader, ReadWrite);
        runtime.FlushBarriers();
        const auto depth_view = CacheView(depth, vk::Format::eR32Sfloat, false);
        const auto ao_view = ao ? CacheView(*ao, ao->info.pixel_format, false) : depth_view;
        const auto target_view = CacheView(target, vk::Format::eR16G16B16A16Sfloat, true);
        PushConstants push = last_composite;
        push.flags = ao ? FlagAo : 0u;
        const bool source = mode == 2;
        if (source) {
            push.param0 = 1.0f;
            push.flags = 0u;
        }
        scheduler.Record([push, depth_view, ao_view, target_view,
                          shown_view = source ? *light.view : *blurred.view,
                          light = light.image->image, pipeline = *show_bounce_pipeline,
                          layout = *pipeline_layout](vk::CommandBuffer cmdbuf) {
            const std::array kept = {ComputeBarrier(light, vk::ImageLayout::eGeneral,
                                                    vk::AccessFlagBits2::eShaderWrite,
                                                    vk::AccessFlagBits2::eShaderRead)};
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                       .pImageMemoryBarriers = kept.data()});
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            PushImages(cmdbuf, layout, {depth_view, shown_view, ao_view}, target_view);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            const auto groups = Groups(push.size, false);
            cmdbuf.dispatch(groups[0], groups[1], 1);
        });
    }
};

BbSsgi::BbSsgi(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
               VideoCore::TextureCache& texture_cache)
    : impl{std::make_unique<Impl>(instance, scheduler, runtime, texture_cache)} {}

BbSsgi::~BbSsgi() = default;

void BbSsgi::OnGBufferDraw(VideoCore::ImageId depth, VideoCore::ImageId normals, float x_sign,
                           float y_sign) {
    auto& s = *impl;
    gbuffer_draw = false;
    if (!depth) {
        return;
    }
    if (s.lighting_done) {
        // A new frame.
        s.lighting_done = false;
        s.depth_id = {};
        s.normals_id = {};
        s.ao_id = {};
        s.depth_area = 0;
        s.pass_has_camera = false;
        s.side_pass = false;
    }
    if (depth == s.depth_id) {
        s.side_pass = false; // the scene's pass again (after a smaller one)
    } else {
        // Another depth target: another G-buffer pass, with its own camera. One smaller than the
        // frame's pass so far is a side view (a water reflection): the scene's stay.
        const auto& image = s.texture_cache.GetImage(depth);
        const u64 area = u64(image.info.size.width) * image.info.size.height;
        if (s.depth_id && area < s.depth_area) {
            s.side_pass = true;
            return;
        }
        s.depth_id = depth;
        s.depth_area = area;
        s.side_pass = false;
        s.pass_has_camera = false;
    }
    // The G-buffer passes differ in their targets; the normals are RGBA8 (0.5 n + 0.5).
    if (normals &&
        s.texture_cache.GetImage(normals).info.pixel_format == vk::Format::eR8G8B8A8Unorm) {
        s.normals_id = normals;
    }
    s.x_sign = x_sign;
    s.y_sign = y_sign;
    gbuffer_draw = true;
}

void BbSsgi::ObserveSceneConstants(const void* data, u64 size) {
    auto& s = *impl;
    if (!gbuffer_draw || s.pass_has_camera || s.side_pass || !data || size < 64 * sizeof(float)) {
        return;
    }
    const auto* f = static_cast<const float*>(data);
    // Scene constants: far plane 3000, 1/far, and the render size (as the better AO reads them).
    if (f[0] != 3000.0f || std::abs(f[1] * f[0] - 1.0f) > 1e-3f || f[4] < 64.0f || f[5] < 64.0f) {
        return;
    }
    if (f[52] == 0.0f || f[57] == 0.0f || !std::isfinite(f[62]) || !std::isfinite(f[63])) {
        return;
    }
    std::memcpy(s.view.data(), f + 8, sizeof(s.view));
    s.proj = {f[52] * s.x_sign, f[57] * s.y_sign, f[62], f[63]};
    s.render_size = {u32(f[4]), u32(f[5])};
    s.pass_has_camera = true;
}

void BbSsgi::OnAoComposite(VideoCore::ImageId target) {
    impl->ao_id = target;
}

void BbSsgi::OnLighting(VideoCore::ImageId diffuse, vk::Format view_format) {
    auto& s = *impl;
    s.LogStats();
    ++s.stats.lighting;
    ++s.lighting_index;
    s.lighting_done = true;
    if (s.failed || !diffuse) {
        return;
    }
    if (!s.depth_id || !s.pass_has_camera) {
        ++(s.depth_id ? s.stats.no_camera : s.stats.no_depth);
        return;
    }
    try {
        s.Run(diffuse, view_format);
    } catch (const std::exception& e) {
        s.failed = true;
        LOG_ERROR(Render_Vulkan, "SSGI: unavailable ({})", e.what());
    }
}

void BbSsgi::OnDiffuseAlbedo(VideoCore::ImageId target) {
    auto& s = *impl;
    // Only the scene's, after its lighting.
    if (s.lighting_done && s.light_frame != s.lighting_index) {
        s.diffuse_albedo = target;
    }
}

void BbSsgi::OnSceneComposite(VideoCore::ImageId target, VideoCore::ImageId self_lit) {
    auto& s = *impl;
    if (s.failed || !target || !s.depth_id) {
        return;
    }
    try {
        // The light the surfaces give off, for the next frame's bounce.
        if (s.lighting_done && s.pass_has_camera && s.diffuse_albedo &&
            s.light_frame != s.lighting_index) {
            s.Capture(s.diffuse_albedo, self_lit);
            s.diffuse_albedo = {};
        }
        const int view =
            EmulatorSettings.IsBbShowThreadedSettings() ? EmulatorSettings.GetBbSsgiDebugView() : 0;
        if (view != 0 && s.bounce_frame == s.lighting_index && s.light_frame == s.lighting_index) {
            s.ShowBounce(target, view);
        }
    } catch (const std::exception& e) {
        s.failed = true;
        LOG_ERROR(Render_Vulkan, "SSGI: unavailable ({})", e.what());
    }
}

} // namespace Vulkan
