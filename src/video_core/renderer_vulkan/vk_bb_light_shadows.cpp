// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/bb_light_shadows_comp.h"
#include "video_core/host_shaders/bb_light_shadows_show_comp.h"
#include "video_core/host_shaders/bb_light_shadows_trace_comp.h"
#include "video_core/renderer_vulkan/vk_bb_light_shadows.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
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

/// As bb_light_shadows.comp's push constants.
struct PushConstants {
    std::array<float, 16> to_clip{};
    std::array<float, 4> viewport{};
    u32 frame{};
    float strength{};
    u32 debug{};
    u32 trace{};                ///< its light draw in the batch (high 16 bits), light, layer
    std::array<u32, 2> fades{}; ///< each layer's light's share of its shadows, a byte each
    u32 origin{};               ///< the first pixel traced (x low 16 bits, y high)
    u32 unused{};
};
static_assert(sizeof(PushConstants) <= 128);

/// As bb_light_shadows.comp's batch: the light draws' camera (28 floats of their buffer 0),
/// then a block for each draw: its rectangle, each light's layer (4 bits each) and how many
/// lights it has, and its lights (position and 1 / radius^2, then colour).
constexpr u32 CameraFloats = 28;
struct DrawBlock {
    std::array<u32, 4> rect{};
    std::array<u32, 4> info{};
    std::array<std::array<float, 4>, 24> lights{};
};
static_assert(sizeof(DrawBlock) == 416);
/// Light draws one batch takes at most: the light draws resolved ahead of the one being recorded
/// (the finish ring's 32) and more.
constexpr u32 MaxBatch = 128;
constexpr u64 BatchBytes = CameraFloats * sizeof(float) + MaxBatch * sizeof(DrawBlock);
static_assert(BatchBytes <= 65536); // one vkCmdUpdateBuffer

constexpr u32 MaxLights = 12; ///< the most a light draw's shader reads (12 blocks)
/// A light's share of its draw's buffer 1: its 16 floats the shader reads, and 20 more after
/// all of them (the buffer's size is the number of lights times this, as logged).
constexpr u64 LightBytes = 144;
/// Lights traced a frame: the four that light the most of the screen, and room for lights
/// fading out while others fade in.
constexpr u32 MaxTraced = 6;
constexpr u32 WantedLights = 4;
constexpr float FadeStep = 0.125f; ///< a light's shadows fade in and out over 8 frames
constexpr u32 NotTraced = 15;      ///< a light's slot when it is not traced
constexpr u32 Removed = 14;        ///< a light's slot when it is taken out (the player light, off)
constexpr u32 MinLightPixels = 48; ///< lights smaller on screen (across) are not traced
constexpr u32 LoggedTraces = 4;    ///< traced lights logged, every 5 s

/// The inverse of a 4x4 matrix (rows), or nothing where it has none.
std::optional<std::array<float, 16>> Inverse(const std::array<float, 16>& m) {
    std::array<double, 32> a{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r * 8 + c] = m[r * 4 + c];
        }
        a[r * 8 + 4 + r] = 1.0;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::abs(a[r * 8 + col]) > std::abs(a[pivot * 8 + col])) {
                pivot = r;
            }
        }
        if (std::abs(a[pivot * 8 + col]) < 1e-12) {
            return std::nullopt;
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                std::swap(a[col * 8 + c], a[pivot * 8 + c]);
            }
        }
        const double inv = 1.0 / a[col * 8 + col];
        for (int c = 0; c < 8; ++c) {
            a[col * 8 + c] *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const double f = a[r * 8 + col];
            for (int c = 0; c < 8; ++c) {
                a[r * 8 + c] -= f * a[col * 8 + c];
            }
        }
    }
    std::array<float, 16> out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = float(a[r * 8 + 4 + c]);
        }
    }
    return out;
}

constexpr auto ShaderRead = vk::AccessFlagBits2::eShaderRead;
constexpr auto ShaderWrite = vk::AccessFlagBits2::eShaderWrite;
constexpr auto ReadWrite = ShaderRead | ShaderWrite;
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;

std::unique_ptr<VideoCore::UniqueImage> CreateImage(const Instance& instance, vk::Format format,
                                                    u32 width, u32 height, u32 layers,
                                                    vk::UniqueImageView& view) {
    auto image =
        std::make_unique<VideoCore::UniqueImage>(instance.GetDevice(), instance.GetAllocator());
    image->Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    view = CheckOrThrow(instance.GetDevice().createImageViewUnique({
        .image = image->image,
        .viewType = layers > 1 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers},
    }));
    return image;
}

vk::ImageMemoryBarrier2 Barrier(vk::Image image, u32 layers, vk::PipelineStageFlags2 src_stage,
                                vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                vk::AccessFlags2 dst_access,
                                vk::ImageLayout old_layout = vk::ImageLayout::eGeneral) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers},
    };
}

/// As the debug view's push constants: each layer's light on screen (x, y, unused, its layer or
/// below 0 for none).
using Markers = std::array<std::array<float, 4>, 6>;

/// A rectangle of pixels: first column and row, and one past the last.
struct Rect {
    u32 x0{}, y0{}, x1{}, y1{};

    [[nodiscard]] u64 Pixels() const {
        return u64(x1 - x0) * (y1 - y0);
    }
};

/// A light as the game's buffers hold it (its position and reach, bit for bit): the same light
/// in all the frame's light draws.
using LightKey = std::array<u32, 4>;

} // Anonymous namespace

struct BbLightShadows::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::TextureCache& texture_cache;
    vk::Device device;
    bool failed{};

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline blend_pipeline;
    vk::UniquePipeline trace_pipeline;
    vk::UniqueDescriptorSetLayout show_desc_layout;
    vk::UniquePipelineLayout show_pipeline_layout;
    vk::UniquePipeline show_pipeline;

    /// The shadowed masks the light draws read as their texture 4 (R8, the masks' size). Each
    /// light draw covers its own tile of the screen, so one image serves them all: a batch of
    /// light draws with tiles apart is blended at once, before the first of them.
    std::unique_ptr<VideoCore::UniqueImage> mask;
    vk::UniqueImageView mask_view;
    /// The traced lights' visibility, a layer each (R8).
    std::unique_ptr<VideoCore::UniqueImage> vis;
    vk::UniqueImageView vis_view;
    std::array<u32, 2> mask_size{};
    /// New images, to clear before their first use (set by the resolve thread, taken by the
    /// first batch recorded after).
    std::atomic<bool> fresh{};
    /// Images replaced (a new size): kept until the light draws already prepared with them
    /// are recorded and done.
    struct Retired {
        std::unique_ptr<VideoCore::UniqueImage> mask, vis, debug;
        vk::UniqueImageView mask_view, vis_view, debug_view;
        u32 frame{};
    };
    std::vector<Retired> retired;
    /// The batch the blend and the traces read (BatchBytes, device local, written in the
    /// command stream before them).
    std::unique_ptr<VideoCore::UniqueBuffer> batch_buffer;

    /// The debug view's record of the frame (RGBA8, the masks' size): the darkest shadow the
    /// frame's blends worked out (red), where they did (green), the game's mask (blue).
    std::unique_ptr<VideoCore::UniqueImage> debug;
    vk::UniqueImageView debug_view;
    bool debug_started{};              ///< this frame's record was started (its first light draw)
    std::atomic<bool> debug_written{}; ///< and has something in it (the debug view shows it)

    struct Trace {
        u32 light{};
        u32 slot{};
        Rect rect{}; // where it reaches
    };
    /// A light draw prepared (on the resolve thread) and waiting for its shadows (on the finish
    /// thread): all its blend and traces need, the images included.
    struct Entry {
        u64 id{};
        u32 frame{};
        PushConstants push{};
        std::array<vk::DescriptorImageInfo, 3> textures{}; // depth, normals, the game's mask
        vk::Image mask{}, vis{}, debug{};
        vk::ImageView mask_view{}, vis_view{}, debug_view{};
        std::array<Trace, MaxTraced> traces{};
        u32 trace_count{};
        DrawBlock block{};
        std::array<float, CameraFloats> camera{};
        bool first_in_frame{};
        bool clear_debug{};
        bool done{}; ///< blended by an earlier draw's batch
    };
    std::mutex queue_mutex;
    std::deque<Entry> queue;
    u64 next_id{1};
    u64 prepared_id{};          ///< the draw being resolved, when Prepare chose it (else 0)
    std::atomic<u64> batches{}; ///< batches recorded (the stats)
    /// Why batches ended (the stats): no light draw was queued after (the CPU wasn't ahead), the
    /// next one's tile overlapped, its textures or frame were others, the batch was full.
    enum class BatchEnd : u32 { NothingQueued, Overlap, Textures, Frame, Full, Count };
    std::array<std::atomic<u64>, u32(BatchEnd::Count)> batch_ends{};
    std::atomic<u64> queued_at_start{}; ///< light draws queued when batches started, summed
    u32 frame{};
    u32 blends_this_frame{};
    /// A light of the frame (each once): position, radius, how much of the screen it lights and
    /// where, and whether it was traced.
    struct Seen {
        LightKey key{};
        std::array<float, 4> light{};
        float importance{};
        Rect rect{};
        bool traced{};
    };
    std::vector<Seen> seen;
    /// The lights traced, a layer each: those that lit the most of the screen last frame (the
    /// ones traced then counting for more, so the choice doesn't flip between frames), found
    /// again by where they are (the camera moves a little in between). A light's shadows fade in
    /// when it is chosen and out when it no longer is, so none pop.
    struct Slot {
        std::array<float, 4> light{}; ///< position, radius (as last found)
        float fade{};
        bool active{};
        bool wanted{};
        bool found{};   ///< found this frame
        LightKey key{}; ///< as found this frame, bit for bit
        std::array<float, 3> colour{};
    };
    std::array<Slot, MaxTraced> slots{};
    PushConstants last_view{}; ///< the frame's last light draw's camera (the debug view's markers)
    std::array<u64, 64> logged{}; ///< light pixel shaders whose buffers were logged
    u32 logged_shaders{};
    u32 traces_logged{};
    /// Log what it does: with the diagnostics in the log, or the debug view, on (each frame).
    bool verbose{};
    bool player_logged{}; ///< the player light was logged since the last stats
    u32 rects_logged{};

    struct Stats {
        u64 frames{}, light_draws{}, blended{}, traced{}, lights{}, untraced{}, not_fitting{};
        u64 blend_pixels{}, trace_pixels{}, whole_screen{};
    } stats;
    std::chrono::steady_clock::time_point next_log{};

    Impl(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
         VideoCore::TextureCache& texture_cache_)
        : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
          texture_cache{texture_cache_}, device{instance_.GetDevice()} {}

    double Screens(u64 pixels) const {
        return mask_size[0] ? double(pixels) / (double(mask_size[0]) * mask_size[1]) : 0.0;
    }

    void LogStats() {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_log) {
            return;
        }
        if (verbose && next_log != std::chrono::steady_clock::time_point{} && stats.frames) {
            const double frames = double(stats.frames);
            LOG_INFO(Render_Vulkan,
                     "Light shadows (5 s): {} frames; a frame: {:.1f} light draws ({:.1f} lights "
                     "each), {:.2f} lights traced ({:.2f} screens), {:.1f} draws blended ({:.2f} "
                     "screens, {:.1f} of them the whole screen) in {:.1f} batches (the GPU's "
                     "stops), {:.1f} left alone (no traced light), {:.1f} not fitting",
                     stats.frames, double(stats.light_draws) / frames,
                     stats.light_draws ? double(stats.lights) / double(stats.light_draws) : 0.0,
                     double(stats.traced) / frames, Screens(stats.trace_pixels) / frames,
                     double(stats.blended) / frames, Screens(stats.blend_pixels) / frames,
                     double(stats.whole_screen) / frames,
                     double(batches.load(std::memory_order_relaxed)) / frames,
                     double(stats.untraced) / frames, double(stats.not_fitting) / frames);
        }
        if (const u64 run = batches.exchange(0, std::memory_order_relaxed); verbose && run != 0) {
            const auto ends = [&](BatchEnd why) {
                return batch_ends[u32(why)].exchange(0, std::memory_order_relaxed);
            };
            LOG_INFO(Render_Vulkan,
                     "Light shadows: batches ended because nothing more was queued {}, a tile "
                     "overlapped {}, other textures {}, the next frame {}, full {}; light draws "
                     "queued when one started {:.1f} on average",
                     ends(BatchEnd::NothingQueued), ends(BatchEnd::Overlap),
                     ends(BatchEnd::Textures), ends(BatchEnd::Frame), ends(BatchEnd::Full),
                     double(queued_at_start.exchange(0, std::memory_order_relaxed)) / double(run));
        }
        // The layers' lights, for the debug view's colours.
        static constexpr std::array<const char*, MaxTraced> Colours = {
            "blue", "green", "yellow", "magenta", "cyan", "orange"};
        for (u32 i = 0; i < MaxTraced; ++i) {
            const auto& slot = slots[i];
            if (verbose && slot.active) {
                LOG_INFO(Render_Vulkan,
                         "Light shadows: layer {} ({}): light ({:.2f}, {:.2f}, {:.2f}) radius "
                         "{:.2f} colour ({:.2f}, {:.2f}, {:.2f}), shadows at {:.2f}{}",
                         i, Colours[i], slot.light[0], slot.light[1], slot.light[2], slot.light[3],
                         slot.colour[0], slot.colour[1], slot.colour[2], slot.fade,
                         slot.wanted ? "" : " (fading out)");
            }
        }
        stats = {};
        traces_logged = 0;
        player_logged = false;
        rects_logged = 0;
        next_log = now + std::chrono::seconds(5);
    }

    vk::UniquePipeline CreateComputePipeline(std::span<const u32> code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto result = device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = layout,
                });
        device.destroyShaderModule(module);
        return CheckOrThrow(std::move(result));
    }

    void CreatePipelines() {
        using T = vk::DescriptorType;
        const auto bind = [](u32 binding, T type) {
            return vk::DescriptorSetLayoutBinding{.binding = binding,
                                                  .descriptorType = type,
                                                  .descriptorCount = 1,
                                                  .stageFlags = vk::ShaderStageFlagBits::eCompute};
        };
        const std::array bindings = {
            bind(0, T::eSampledImage), bind(1, T::eSampledImage),  bind(2, T::eSampledImage),
            bind(3, T::eStorageImage), bind(4, T::eStorageBuffer), bind(6, T::eStorageImage),
            bind(7, T::eStorageImage), bind(8, T::eSampledImage),
        };
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
        blend_pipeline = CreateComputePipeline(BB_LIGHT_SHADOWS_COMP, *pipeline_layout);
        trace_pipeline = CreateComputePipeline(BB_LIGHT_SHADOWS_TRACE_COMP, *pipeline_layout);

        const std::array show_bindings = {bind(3, T::eStorageImage), bind(6, T::eStorageImage)};
        show_desc_layout = CheckOrThrow(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(show_bindings.size()),
            .pBindings = show_bindings.data(),
        }));
        const auto show_set = *show_desc_layout;
        const vk::PushConstantRange markers{
            .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(Markers)};
        show_pipeline_layout = CheckOrThrow(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &show_set,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &markers,
        }));
        show_pipeline = CreateComputePipeline(BB_LIGHT_SHADOWS_SHOW_COMP, *show_pipeline_layout);

        batch_buffer = std::make_unique<VideoCore::UniqueBuffer>(device, instance.GetAllocator());
        vk::BufferCreateInfo buffer_ci{
            .size = BatchBytes,
            .usage =
                vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
        };
        batch_buffer->Create(buffer_ci, VideoCore::MemoryType::DeviceLocal, nullptr,
                             "light shadows batch");
    }

    void EnsureImages(u32 width, u32 height) {
        if (mask && mask_size[0] == width && mask_size[1] == height) {
            return;
        }
        if (mask) {
            // Light draws prepared with the old ones may not be recorded yet.
            retired.push_back({std::move(mask), std::move(vis), std::move(debug),
                               std::move(mask_view), std::move(vis_view), std::move(debug_view),
                               frame});
        }
        mask = CreateImage(instance, vk::Format::eR8Unorm, width, height, 1, mask_view);
        vis = CreateImage(instance, vk::Format::eR8Unorm, width, height, MaxTraced, vis_view);
        debug = CreateImage(instance, vk::Format::eR8G8B8A8Unorm, width, height, 1, debug_view);
        mask_size = {width, height};
        fresh.store(true, std::memory_order_release);
    }

    /// A view position's pixel position (CPU side of the shader's ToScreen); false where it is
    /// behind the camera.
    static bool ToScreen(const PushConstants& p, const std::array<float, 3>& pos,
                         std::array<float, 2>& out) {
        std::array<float, 4> clip{};
        for (int r = 0; r < 4; ++r) {
            clip[r] = p.to_clip[r * 4] * pos[0] + p.to_clip[r * 4 + 1] * pos[1] +
                      p.to_clip[r * 4 + 2] * pos[2] + p.to_clip[r * 4 + 3];
        }
        if (!(clip[3] > 1e-6f)) {
            return false;
        }
        out = {clip[0] / clip[3] * p.viewport[0] + p.viewport[1],
               clip[1] / clip[3] * p.viewport[2] + p.viewport[3]};
        return true;
    }

    /// Whether a light is the player's: the soft white light the game keeps around the player
    /// character so it shows in the dark (radius 3.5, grey 0.47 in the logs), always a few
    /// metres in front of the camera near the middle of the screen, where the character is.
    static bool IsPlayerLight(const float* l, float radius) {
        const float lo = std::min({l[4], l[5], l[6]});
        const float hi = std::max({l[4], l[5], l[6]});
        return hi > 0.0f && hi - lo <= 0.02f * hi && std::abs(radius - 3.5f) < 0.05f &&
               std::abs(l[0]) < 1.5f && l[1] > -1.0f && l[1] < 1.5f && l[2] > 1.0f && l[2] < 8.0f;
    }

    /// Pixel bounds to whole pixels, a little larger, on the image.
    static Rect ToRect(float lx, float ly, float hx, float hy, float width, float height) {
        return Rect{u32(std::clamp(std::floor(lx) - 2.0f, 0.0f, width)),
                    u32(std::clamp(std::floor(ly) - 2.0f, 0.0f, height)),
                    u32(std::clamp(std::ceil(hx) + 2.0f, 0.0f, width)),
                    u32(std::clamp(std::ceil(hy) + 2.0f, 0.0f, height))};
    }

    /// Whether a light of this frame is one chosen from last frame's (`c`: position, radius).
    static bool Matches(const std::array<float, 4>& c, const float* l, float radius) {
        const float dx = l[0] - c[0], dy = l[1] - c[1], dz = l[2] - c[2];
        const float tolerance = 0.3f + 0.1f * std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        return std::abs(radius - c[3]) <= 0.02f * c[3] + 1e-3f &&
               dx * dx + dy * dy + dz * dz <= tolerance * tolerance;
    }

    /// How much a light's shadows would show: the share of the screen its sphere's bounds cover
    /// (`rect`), times its brightness, times how close it is for its size (a light the camera is
    /// in or near lights all it sees brightly; one far away only dimly). Smooth as the view
    /// turns, so lights that light the whole screen don't tie. 0 where it is too small on
    /// screen to trace.
    static float Importance(const PushConstants& push, const float* l, float width, float height,
                            Rect& rect) {
        const float radius = 1.0f / std::sqrt(l[3]);
        // The sphere's bounding box, cut at a plane just in front of the camera: the box that is
        // left projects to a rectangle that holds all the light reaches on screen.
        constexpr float Near = 0.05f;
        if (!std::isfinite(radius) || l[2] + radius <= Near) {
            return 0.0f;
        }
        const float z_near = std::max(l[2] - radius, Near);
        const float z_far = l[2] + radius;
        float lx = width, ly = height, hx = 0.0f, hy = 0.0f;
        for (int corner = 0; corner < 8; ++corner) {
            const std::array<float, 3> pos = {l[0] + ((corner & 1) ? radius : -radius),
                                              l[1] + ((corner & 2) ? radius : -radius),
                                              (corner & 4) ? z_far : z_near};
            std::array<float, 2> px{};
            if (!ToScreen(push, pos, px)) {
                lx = ly = 0.0f;
                hx = width;
                hy = height;
                break;
            }
            lx = std::min(lx, px[0]);
            hx = std::max(hx, px[0]);
            ly = std::min(ly, px[1]);
            hy = std::max(hy, px[1]);
        }
        rect = ToRect(lx, ly, hx, hy, width, height);
        const float across = float(rect.x1) - float(rect.x0);
        const float down = float(rect.y1) - float(rect.y0);
        if (across < float(MinLightPixels) || down < float(MinLightPixels)) {
            return 0.0f;
        }
        const float brightness = 0.3f * l[4] + 0.59f * l[5] + 0.11f * l[6];
        const float dist2 = l[0] * l[0] + l[1] * l[1] + l[2] * l[2];
        const float closeness = radius * radius / (radius * radius + dist2);
        const float share = across * down / (width * height);
        return 1000.0f * share * closeness * std::max(brightness, 0.0f);
    }

    vk::ImageView Prepare(u64 ps_hash, std::span<const vk::DescriptorImageInfo> textures,
                          std::span<const VideoCore::ImageId> ids, const Buffer& camera,
                          const Buffer& lights, const AmdGpu::Regs& regs, const DrawArea& area) {
        prepared_id = 0;
        if (textures.size() < 5 || ids.size() < 5 || !ids[0] || !ids[1] || !ids[4]) {
            return {};
        }
        const auto& depth = texture_cache.GetImage(ids[0]).info;
        const auto& normals = texture_cache.GetImage(ids[1]).info;
        const auto& game_mask = texture_cache.GetImage(ids[4]).info;
        // The light draws' textures: the depth copy, the normals, the R8 mask, all one size.
        if (depth.pixel_format != vk::Format::eR32Sfloat ||
            normals.pixel_format != vk::Format::eR8G8B8A8Unorm ||
            game_mask.pixel_format != vk::Format::eR8Unorm ||
            depth.size.width != game_mask.size.width ||
            depth.size.height != game_mask.size.height ||
            normals.size.width != game_mask.size.width ||
            normals.size.height != game_mask.size.height || game_mask.num_samples != 1) {
            return {};
        }
        ++stats.light_draws;
        if (!camera.buffer || !lights.buffer || !camera.guest || !lights.guest ||
            camera.guest_size < CameraFloats * sizeof(float) || lights.guest_size < LightBytes ||
            lights.guest_size % LightBytes != 0) {
            ++stats.not_fitting;
            return {};
        }
        std::array<float, 16> view_from_ndc{};
        std::memcpy(view_from_ndc.data(), camera.guest, sizeof(view_from_ndc));
        const auto to_clip = Inverse(view_from_ndc);
        if (!to_clip) {
            ++stats.not_fitting;
            return {};
        }
        PushConstants push{};
        push.to_clip = *to_clip;
        // The light draws cover the screen in the camera's ndc (their position input is it):
        // pixels map to it over the whole image, the viewport's signs giving the axes'
        // directions (its registers can hold another size than the image's).
        const auto& vp = regs.viewports[0];
        const auto& control = regs.viewport_control;
        const float x_sign = control.xscale_enable && vp.xscale < 0.0f ? -1.0f : 1.0f;
        const float y_sign = control.yscale_enable && vp.yscale < 0.0f ? -1.0f : 1.0f;
        const float width = float(game_mask.size.width);
        const float height = float(game_mask.size.height);
        push.viewport = {width * 0.5f * x_sign, width * 0.5f, height * 0.5f * y_sign,
                         height * 0.5f};

        const u32 count = std::min<u32>(u32(lights.guest_size / LightBytes), MaxLights);
        if (verbose && logged_shaders < logged.size() &&
            std::ranges::find(logged, ps_hash) == logged.end()) {
            logged[logged_shaders++] = ps_hash;
            LOG_INFO(Render_Vulkan,
                     "Light shadows: light draws of pixel shader {:#x}, lights buffer {} bytes "
                     "({} lights), camera buffer {} bytes, {}x{}",
                     ps_hash, lights.guest_size, count, camera.guest_size, game_mask.size.width,
                     game_mask.size.height);
        }
        stats.lights += count;

        // Each light's layer: the frame's traced lights are traced once, before the first of
        // their draws; the rest count as unshadowed.
        const auto* light_data = static_cast<const float*>(lights.guest);
        const bool shadows = EmulatorSettings.IsBbLightShadows();
        const bool player_light = EmulatorSettings.IsBbPlayerLight();
        std::array<Trace, MaxTraced> traces{};
        u32 trace_count = 0;
        u32 traced_here = 0;
        std::array<u32, 2> light_slots = {0xFFFFFFFFu, 0xFFFFFFFFu};
        for (u32 i = 0; i < count; ++i) {
            const float* l = light_data + i * 16;
            if (!(l[3] > 0.0f) || !std::isfinite(l[0]) || !std::isfinite(l[1]) ||
                !std::isfinite(l[2])) {
                continue;
            }
            const float radius = 1.0f / std::sqrt(l[3]);
            if (IsPlayerLight(l, radius)) {
                // Never shadowed (the character would shadow all around it); taken out when off.
                if (!player_light) {
                    ++traced_here;
                    auto& word = light_slots[i / 8];
                    const u32 shift = (i % 8) * 4;
                    word = (word & ~(15u << shift)) | (Removed << shift);
                    if (verbose && !player_logged) {
                        player_logged = true;
                        LOG_INFO(Render_Vulkan,
                                 "Light shadows: player light at ({:.2f}, {:.2f}, {:.2f}) radius "
                                 "{:.2f} colour ({:.2f}, {:.2f}, {:.2f}) taken out",
                                 l[0], l[1], l[2], radius, l[4], l[5], l[6]);
                    }
                }
                continue;
            }
            if (!shadows) {
                continue;
            }
            LightKey key{};
            std::memcpy(key.data(), l, sizeof(key));
            auto it = std::ranges::find(seen, key, &Seen::key);
            if (it == seen.end()) {
                Seen entry{.key = key, .light = {l[0], l[1], l[2], 1.0f / std::sqrt(l[3])}};
                entry.importance = Importance(push, l, width, height, entry.rect);
                it = seen.insert(seen.end(), entry);
            }
            u32 slot = NotTraced;
            for (u32 s = 0; s < MaxTraced; ++s) {
                if (slots[s].found && slots[s].key == key) {
                    slot = s;
                    break;
                }
            }
            for (u32 s = 0; s < MaxTraced && slot == NotTraced; ++s) {
                auto& candidate = slots[s];
                if (!candidate.active || candidate.found ||
                    !Matches(candidate.light, l, it->light[3])) {
                    continue;
                }
                slot = s;
                candidate.found = true;
                candidate.key = key;
                candidate.light = it->light;
                candidate.colour = {l[4], l[5], l[6]};
                traces[trace_count++] = {i, s, it->rect};
                ++stats.traced;
                stats.trace_pixels += it->rect.Pixels();
                if (verbose && traces_logged < LoggedTraces) {
                    ++traces_logged;
                    LOG_INFO(Render_Vulkan,
                             "Light shadows: traced light ({:.2f}, {:.2f}, {:.2f}) radius {:.2f} "
                             "colour ({:.2f}, {:.2f}, {:.2f}), importance {:.1f}, pixels "
                             "{}..{} x {}..{}, layer {} at {:.2f}, frame {}",
                             l[0], l[1], l[2], it->light[3], l[4], l[5], l[6], it->importance,
                             it->rect.x0, it->rect.x1, it->rect.y0, it->rect.y1, s, candidate.fade,
                             frame);
                }
            }
            if (slot == NotTraced) {
                continue;
            }
            it->traced = true;
            ++traced_here;
            auto& word = light_slots[i / 8];
            const u32 shift = (i % 8) * 4;
            word = (word & ~(15u << shift)) | (slot << shift);
        }
        if (traced_here == 0) {
            ++stats.untraced;
            return {};
        }
        if (!blend_pipeline) {
            CreatePipelines();
        }
        EnsureImages(game_mask.size.width, game_mask.size.height);

        // The rectangle the light draw covers: its scissor, and its quad's bounds where they are
        // known.
        const auto clamp_x = [&](float v) { return u32(std::clamp(v, 0.0f, width)); };
        const auto clamp_y = [&](float v) { return u32(std::clamp(v, 0.0f, height)); };
        Rect rect{clamp_x(float(area.scissor[0])), clamp_y(float(area.scissor[1])),
                  clamp_x(float(area.scissor[2])), clamp_y(float(area.scissor[3]))};
        if (area.quad) {
            const auto& q = *area.quad;
            rect.x0 = std::max(rect.x0, clamp_x(std::floor(q[0]) - 1.0f));
            rect.y0 = std::max(rect.y0, clamp_y(std::floor(q[1]) - 1.0f));
            rect.x1 = std::min(rect.x1, clamp_x(std::ceil(q[2]) + 1.0f));
            rect.y1 = std::min(rect.y1, clamp_y(std::ceil(q[3]) + 1.0f));
        }
        if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
            ++stats.untraced;
            return {};
        }
        if (rect.Pixels() == u64(game_mask.size.width) * game_mask.size.height) {
            ++stats.whole_screen;
        }
        stats.blend_pixels += rect.Pixels();
        if (verbose && rects_logged < 6) {
            ++rects_logged;
            const auto quad = area.quad.value_or(std::array<float, 4>{});
            LOG_INFO(Render_Vulkan,
                     "Light shadows: draw {:#x} scissor {}..{} x {}..{}, quad {} ({:.0f}..{:.0f} "
                     "x {:.0f}..{:.0f}): pixels {}..{} x {}..{}",
                     ps_hash, area.scissor[0], area.scissor[2], area.scissor[1], area.scissor[3],
                     area.quad ? "read" : "not read", quad[0], quad[2], quad[1], quad[3], rect.x0,
                     rect.x1, rect.y0, rect.y1);
        }

        push.frame = frame;
        for (u32 i = 0; i < MaxTraced; ++i) {
            const u32 fade = u32(std::lround(std::clamp(slots[i].fade, 0.0f, 1.0f) * 255.0f));
            push.fades[i / 4] |= fade << ((i % 4) * 8);
        }
        push.strength =
            shadows ? std::clamp(float(EmulatorSettings.GetBbLightShadowsStrength()) * 0.01f, 0.0f,
                                 1.0f)
                    : 0.0f;
        push.debug = EmulatorSettings.IsBbLightShadowsDebug() ? 1u : 0u;
        last_view = push;

        // All the batch needs, copied now (the game rewrites its buffers for the next frame).
        Entry entry{
            .id = next_id++,
            .frame = frame,
            .push = push,
            .textures = {textures[0], textures[1], textures[4]},
            .mask = mask->image,
            .vis = vis->image,
            .debug = debug->image,
            .mask_view = *mask_view,
            .vis_view = *vis_view,
            .debug_view = *debug_view,
            .traces = traces,
            .trace_count = trace_count,
            .first_in_frame = blends_this_frame == 0,
            .clear_debug = push.debug != 0 && !std::exchange(debug_started, true),
        };
        entry.block.rect = {rect.x0, rect.y0, rect.x1, rect.y1};
        entry.block.info = {light_slots[0], light_slots[1], count, 0};
        for (u32 i = 0; i < count; ++i) {
            const float* l = light_data + i * 16;
            entry.block.lights[i * 2] = {l[0], l[1], l[2], l[3]};
            entry.block.lights[i * 2 + 1] = {l[4], l[5], l[6], 0.0f};
        }
        std::memcpy(entry.camera.data(), camera.guest, sizeof(entry.camera));
        prepared_id = entry.id;
        {
            std::scoped_lock lock{queue_mutex};
            queue.push_back(entry);
        }
        ++blends_this_frame;
        ++stats.blended;
        return *mask_view;
    }

    static bool Overlaps(const std::array<u32, 4>& a, const std::array<u32, 4>& b) {
        return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
    }

    /// Light draw `id` is about to be recorded: unless an earlier draw's batch blended it, it
    /// and the light draws prepared after it (their tiles apart, the frame and textures the
    /// same) are blended now, in one batch, and their lights traced where first needed.
    void RunPending(u64 id) {
        std::vector<Entry> batch;
        {
            std::scoped_lock lock{queue_mutex};
            // Light draws prepared but left out (their draw skipped after).
            while (!queue.empty() && queue.front().id < id) {
                queue.pop_front();
            }
            if (queue.empty() || queue.front().id != id) {
                return;
            }
            if (queue.front().done) {
                queue.pop_front();
                return; // blended with an earlier draw's batch: no stop
            }
            batch.reserve(MaxBatch);
            queued_at_start.fetch_add(queue.size(), std::memory_order_relaxed);
            BatchEnd end = BatchEnd::NothingQueued;
            const auto stop = [&](BatchEnd why) {
                end = why;
                return true;
            };
            for (auto& entry : queue) {
                if (batch.size() == MaxBatch && stop(BatchEnd::Full)) {
                    break;
                }
                if (!batch.empty()) {
                    const auto& first = batch.front();
                    const auto same_view = [](const vk::DescriptorImageInfo& a,
                                              const vk::DescriptorImageInfo& b) {
                        return a.imageView == b.imageView;
                    };
                    if ((entry.frame != first.frame && stop(BatchEnd::Frame)) ||
                        ((entry.mask != first.mask ||
                          !same_view(entry.textures[0], first.textures[0]) ||
                          !same_view(entry.textures[1], first.textures[1]) ||
                          !same_view(entry.textures[2], first.textures[2])) &&
                         stop(BatchEnd::Textures)) ||
                        (std::ranges::any_of(batch,
                                             [&](const Entry& other) {
                                                 return Overlaps(entry.block.rect,
                                                                 other.block.rect);
                                             }) &&
                         stop(BatchEnd::Overlap))) {
                        break;
                    }
                }
                entry.done = true;
                batch.push_back(entry);
            }
            queue.pop_front();
            batch_ends[u32(end)].fetch_add(1, std::memory_order_relaxed);
        }
        batches.fetch_add(1, std::memory_order_relaxed);
        RecordBatch(batch);
    }

    void RecordBatch(const std::vector<Entry>& batch) {
        const Entry& first = batch.front();
        auto* profiler = GpuProfiler::Get();
        if (profiler && profiler->Records(&scheduler)) {
            profiler->Mark(0x5AD0'BA7Cull,
                           [] { return std::string{"light shadows: a batch of light tiles"}; });
        }
        scheduler.EndRendering();
        const bool first_use = fresh.exchange(false, std::memory_order_acq_rel);
        bool first_in_frame = false, clear_debug = false;
        u32 trace_count = 0;
        u32 max_groups_x = 0, max_groups_y = 0;
        for (const auto& entry : batch) {
            first_in_frame |= entry.first_in_frame;
            clear_debug |= entry.clear_debug;
            trace_count += entry.trace_count;
            const auto& rect = entry.block.rect;
            max_groups_x = std::max(max_groups_x, (rect[2] - rect[0] + 7) / 8);
            max_groups_y = std::max(max_groups_y, (rect[3] - rect[1] + 7) / 8);
        }
        if (first.push.debug) {
            debug_written.store(true, std::memory_order_relaxed);
        }
        // The batch's data, written in the command stream before the passes that read it.
        const u32 bytes = u32(CameraFloats * sizeof(float) + batch.size() * sizeof(DrawBlock));
        std::vector<u8> data(bytes);
        std::memcpy(data.data(), first.camera.data(), CameraFloats * sizeof(float));
        for (u32 i = 0; i < batch.size(); ++i) {
            std::memcpy(data.data() + CameraFloats * sizeof(float) + i * sizeof(DrawBlock),
                        &batch[i].block, sizeof(DrawBlock));
        }
        // The traces: each draw's own lights (its index in the batch).
        std::vector<std::pair<u32, Trace>> traces;
        traces.reserve(trace_count);
        for (u32 i = 0; i < batch.size(); ++i) {
            for (u32 t = 0; t < batch[i].trace_count; ++t) {
                traces.emplace_back(i, batch[i].traces[t]);
            }
        }
        const u32 count = u32(batch.size());
        scheduler.RecordData(
            [push = first.push, textures = first.textures, mask = first.mask,
             mask_view = first.mask_view, vis = first.vis, vis_view = first.vis_view,
             debug_image = first.debug, debug_view = first.debug_view, traces = std::move(traces),
             first_use, first_in_frame, clear_debug, count, bytes, max_groups_x, max_groups_y,
             buffer = batch_buffer->buffer, blend = *blend_pipeline, trace = *trace_pipeline,
             layout = *pipeline_layout](vk::CommandBuffer cmdbuf, const u8* data) {
                const auto range =
                    vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
                if (first_use) {
                    // Every pixel 1 (no shadow) once: a light draw reads the whole image.
                    const std::array to_general = {
                        Barrier(mask, 1, vk::PipelineStageFlagBits2::eNone, {},
                                vk::PipelineStageFlagBits2::eClear,
                                vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eUndefined),
                        Barrier(vis, MaxTraced, vk::PipelineStageFlagBits2::eNone, {}, Compute,
                                ReadWrite, vk::ImageLayout::eUndefined),
                    };
                    cmdbuf.pipelineBarrier2(
                        vk::DependencyInfo{.imageMemoryBarrierCount = u32(to_general.size()),
                                           .pImageMemoryBarriers = to_general.data()});
                    const vk::ClearColorValue one{std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}};
                    cmdbuf.clearColorImage(mask, vk::ImageLayout::eGeneral, one, range);
                }
                if (clear_debug) {
                    // No shadow, not worked out (min() keeps the darkest).
                    const auto to_clear = Barrier(
                        debug_image, 1, Compute, ReadWrite, vk::PipelineStageFlagBits2::eClear,
                        vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eUndefined);
                    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                               .pImageMemoryBarriers = &to_clear});
                    const vk::ClearColorValue start{std::array<float, 4>{1.0f, 0.0f, 1.0f, 0.0f}};
                    cmdbuf.clearColorImage(debug_image, vk::ImageLayout::eGeneral, start, range);
                }
                // The last batch's reads of the batch buffer, then its new data.
                const vk::BufferMemoryBarrier2 to_write{
                    .srcStageMask = Compute,
                    .srcAccessMask = ShaderRead,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = buffer,
                    .offset = 0,
                    .size = VK_WHOLE_SIZE,
                };
                cmdbuf.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                                           .pBufferMemoryBarriers = &to_write});
                cmdbuf.updateBuffer(buffer, 0, bytes, data);
                // What the game wrote before its lighting (the depth copy, the normals, the
                // masks), once a frame; the batch data; the light draws' reads of the mask and
                // the last batch's of the layers; the debug record.
                std::array<vk::MemoryBarrier2, 2> memory{};
                u32 memory_count = 0;
                memory[memory_count++] = vk::MemoryBarrier2{
                    .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask = Compute,
                    .dstAccessMask = ShaderRead,
                };
                if (first_in_frame || first_use) {
                    memory[memory_count++] = vk::MemoryBarrier2{
                        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                        .dstStageMask = Compute,
                        .dstAccessMask = ShaderRead,
                    };
                }
                std::array<vk::ImageMemoryBarrier2, 3> before{};
                u32 before_count = 0;
                before[before_count++] =
                    Barrier(mask, 1,
                            vk::PipelineStageFlagBits2::eFragmentShader |
                                vk::PipelineStageFlagBits2::eClear,
                            ShaderRead | vk::AccessFlagBits2::eTransferWrite, Compute, ShaderWrite);
                if (!traces.empty()) {
                    before[before_count++] =
                        Barrier(vis, MaxTraced, Compute, ShaderRead, Compute, ShaderWrite);
                }
                if (push.debug) {
                    before[before_count++] = Barrier(
                        debug_image, 1, Compute | vk::PipelineStageFlagBits2::eClear,
                        ShaderWrite | vk::AccessFlagBits2::eTransferWrite, Compute, ReadWrite);
                }
                cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = memory_count,
                                                           .pMemoryBarriers = memory.data(),
                                                           .imageMemoryBarrierCount = before_count,
                                                           .pImageMemoryBarriers = before.data()});

                const vk::DescriptorImageInfo mask_storage{
                    .imageView = mask_view, .imageLayout = vk::ImageLayout::eGeneral};
                const vk::DescriptorImageInfo debug_storage{
                    .imageView = debug_view, .imageLayout = vk::ImageLayout::eGeneral};
                const vk::DescriptorImageInfo vis_info{.imageView = vis_view,
                                                       .imageLayout = vk::ImageLayout::eGeneral};
                const vk::DescriptorBufferInfo batch_info{buffer, 0, bytes};
                const auto image = [](u32 binding, vk::DescriptorType type,
                                      const vk::DescriptorImageInfo* info) {
                    return vk::WriteDescriptorSet{.dstBinding = binding,
                                                  .descriptorCount = 1,
                                                  .descriptorType = type,
                                                  .pImageInfo = info};
                };
                using T = vk::DescriptorType;
                const std::array writes = {
                    image(0, T::eSampledImage, &textures[0]),
                    image(1, T::eSampledImage, &textures[1]),
                    image(2, T::eSampledImage, &textures[2]),
                    image(3, T::eStorageImage, &mask_storage),
                    vk::WriteDescriptorSet{.dstBinding = 4,
                                           .descriptorCount = 1,
                                           .descriptorType = T::eStorageBuffer,
                                           .pBufferInfo = &batch_info},
                    image(6, T::eStorageImage, &debug_storage),
                    image(7, T::eStorageImage, &vis_info),
                    image(8, T::eSampledImage, &vis_info),
                };

                if (!traces.empty()) {
                    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, trace);
                    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
                    for (const auto& [draw, t] : traces) {
                        PushConstants trace_push = push;
                        trace_push.trace = (draw << 16) | (t.light << 8) | t.slot;
                        trace_push.origin = t.rect.x0 | (t.rect.y0 << 16);
                        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                                             sizeof(trace_push), &trace_push);
                        cmdbuf.dispatch((t.rect.x1 - t.rect.x0 + 7) / 8,
                                        (t.rect.y1 - t.rect.y0 + 7) / 8, 1);
                    }
                    const auto traced =
                        Barrier(vis, MaxTraced, Compute, ShaderWrite, Compute, ShaderRead);
                    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                               .pImageMemoryBarriers = &traced});
                }
                // Every light draw of the batch at once: a layer of workgroups each.
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, blend);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                                     &push);
                cmdbuf.dispatch(max_groups_x, max_groups_y, count);
                const auto after = Barrier(mask, 1, Compute, ShaderWrite,
                                           vk::PipelineStageFlagBits2::eFragmentShader, ShaderRead);
                cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                           .pImageMemoryBarriers = &after});
            },
            data.data(), bytes);
    }

    /// The debug view over the finished scene (`target_id`), when the frame recorded one.
    void Show(VideoCore::ImageId target_id) {
        if (!debug_written.exchange(false, std::memory_order_relaxed) || !show_pipeline || !debug) {
            return;
        }
        auto& target = texture_cache.GetImage(target_id);
        if (target.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
            !(target.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            return;
        }
        scheduler.EndRendering();
        runtime.Transit(&target, vk::ImageLayout::eGeneral, Compute, ReadWrite);
        runtime.FlushBarriers();
        VideoCore::ImageViewInfo info{};
        info.type = AmdGpu::ImageType::Color2D;
        info.format = vk::Format::eR16G16B16A16Sfloat;
        info.is_storage = true;
        const auto target_view = *target.FindView(info, false).image_view;
        const u32 width = target.info.size.width;
        const u32 height = target.info.size.height;
        Markers markers{};
        for (u32 i = 0; i < markers.size(); ++i) {
            markers[i] = {0.0f, 0.0f, 0.0f, -1.0f};
            const auto& slot = slots[i];
            std::array<float, 2> px{};
            if (slot.active && slot.found &&
                ToScreen(last_view, {slot.light[0], slot.light[1], slot.light[2]}, px)) {
                markers[i] = {px[0], px[1], 0.0f, float(i)};
            }
        }
        scheduler.Record([width, height, target_view, markers, debug_image = debug->image,
                          debug_view = *debug_view, pipeline = *show_pipeline,
                          layout = *show_pipeline_layout](vk::CommandBuffer cmdbuf) {
            const auto written = Barrier(debug_image, 1, Compute, ShaderWrite, Compute, ShaderRead);
            cmdbuf.pipelineBarrier2(
                vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &written});
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            const vk::DescriptorImageInfo scene{.imageView = target_view,
                                                .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo record{.imageView = debug_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
            const std::array writes = {
                vk::WriteDescriptorSet{.dstBinding = 3,
                                       .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eStorageImage,
                                       .pImageInfo = &scene},
                vk::WriteDescriptorSet{.dstBinding = 6,
                                       .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eStorageImage,
                                       .pImageInfo = &record},
            };
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(markers),
                                 &markers);
            cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
        });
    }
};

BbLightShadows::BbLightShadows(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                               VideoCore::TextureCache& texture_cache)
    : impl{std::make_unique<Impl>(instance, scheduler, runtime, texture_cache)} {}

BbLightShadows::~BbLightShadows() = default;

void BbLightShadows::OnLightingPass() {
    auto& s = *impl;
    s.verbose = EmulatorSettings.IsThreadedRendererDiagnostics() ||
                EmulatorSettings.IsBbLightShadowsDebug();
    s.debug_started = false;
    // Images replaced a few frames ago: the light draws prepared with them are recorded by now,
    // and freed once the GPU is done with them.
    std::erase_if(s.retired, [&](auto& old) {
        if (s.frame - old.frame < 8) {
            return false;
        }
        s.scheduler.DeferOperation([old = std::move(old)] {});
        return true;
    });
    s.LogStats();
    ++s.stats.frames;
    ++s.frame;
    s.blends_this_frame = 0;
    // The lights to trace this frame: the four of last frame's that lit the most of the screen,
    // those traced then counting for half as much again (a flickering flame, or one a little
    // smaller on screen, keeps its shadows).
    auto& seen = s.seen;
    const auto score = [](const auto& e) { return e.importance * (e.traced ? 1.5f : 1.0f); };
    const u32 count = std::min<u32>(u32(seen.size()), WantedLights);
    std::partial_sort(seen.begin(), seen.begin() + count, seen.end(),
                      [&](const auto& a, const auto& b) { return score(a) > score(b); });
    for (auto& slot : s.slots) {
        slot.wanted = false;
    }
    for (u32 i = 0; i < count; ++i) {
        const auto& light = seen[i];
        if (light.importance <= 0.0f) {
            continue;
        }
        const float position[3] = {light.light[0], light.light[1], light.light[2]};
        auto slot = std::ranges::find_if(s.slots, [&](const auto& slot) {
            return slot.active && !slot.wanted &&
                   Impl::Matches(slot.light, position, light.light[3]);
        });
        if (slot == s.slots.end()) {
            slot = std::ranges::find_if(s.slots, [](const auto& slot) { return !slot.active; });
            if (slot == s.slots.end()) {
                continue; // every layer is busy fading out: next frame
            }
            *slot = {.light = light.light, .active = true};
        }
        slot->wanted = true;
    }
    for (auto& slot : s.slots) {
        slot.found = false;
        if (!slot.active) {
            continue;
        }
        slot.fade = std::clamp(slot.fade + (slot.wanted ? FadeStep : -FadeStep), 0.0f, 1.0f);
        slot.active = slot.fade > 0.0f;
    }
    seen.clear();
}

vk::ImageView BbLightShadows::Prepare(u64 ps_hash,
                                      std::span<const vk::DescriptorImageInfo> textures,
                                      std::span<const VideoCore::ImageId> ids, const Buffer& camera,
                                      const Buffer& lights, const AmdGpu::Regs& regs,
                                      const DrawArea& area) {
    auto& s = *impl;
    if (s.failed) {
        return {};
    }
    try {
        return s.Prepare(ps_hash, textures, ids, camera, lights, regs, area);
    } catch (const std::exception& e) {
        s.failed = true;
        s.prepared_id = 0;
        LOG_ERROR(Render_Vulkan, "Light shadows: unavailable ({})", e.what());
        return {};
    }
}

bool BbLightShadows::HasPending() const noexcept {
    return impl->prepared_id != 0;
}

u64 BbLightShadows::PreparedId() const noexcept {
    return impl->prepared_id;
}

void BbLightShadows::RunPending(u64 id) {
    auto& s = *impl;
    if (s.failed) {
        return;
    }
    try {
        s.RunPending(id);
    } catch (const std::exception& e) {
        s.failed = true;
        LOG_ERROR(Render_Vulkan, "Light shadows: unavailable ({})", e.what());
    }
}

void BbLightShadows::Show(VideoCore::ImageId target) {
    auto& s = *impl;
    if (s.failed) {
        return;
    }
    try {
        s.Show(target);
    } catch (const std::exception& e) {
        s.failed = true;
        LOG_ERROR(Render_Vulkan, "Light shadows: debug view unavailable ({})", e.what());
    }
}

void BbLightShadows::ClearPending() noexcept {
    impl->prepared_id = 0;
}

} // namespace Vulkan
