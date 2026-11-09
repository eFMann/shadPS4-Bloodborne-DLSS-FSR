// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace AmdGpu {
union Regs;
}

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;

/// Light shadows: screen-space shadows for Bloodborne's lamps, fires, candles and torch, which
/// cast none. Its lights are drawn after the lighting pass as screen rectangles, one to twelve
/// point lights a draw (pixel shaders that read the depth copy as texture 0, the G-buffer normals
/// as texture 1 and a per-pixel mask as texture 4 that multiplies all the draw adds; the lights,
/// 16 floats each, in their buffer 1: view position, 1 / radius squared, colour; the camera in
/// their buffer 0). The screen is lit in tiles, a draw for each with the lights that reach it, so
/// a light is in many draws. The few lights that light the most of the screen are traced once a
/// frame (bb_light_shadows.comp, TRACE), each into a layer of their own; before each draw with
/// one of them, its mask times its lights' shadows is written to an image of ours, which the draw
/// reads as its texture 4 instead: the game's shader then lights the scene with the shadows in,
/// its colours and falloff intact.
class BbLightShadows {
public:
    /// A buffer the light draw binds: where the GPU reads it and where its guest data is.
    struct Buffer {
        vk::Buffer buffer{};
        u64 binding_offset{};
        u64 size{};
        u32 dword_offset{};
        const void* guest{};
        u64 guest_size{};
    };

    /// Where a light draw draws: its scissor, and the bounds of its quad (its vertices' positions
    /// through the viewport) when they could be read, pixels (x, y least, then one past most).
    struct DrawArea {
        std::array<s32, 4> scissor{};
        std::optional<std::array<float, 4>> quad;
    };

    BbLightShadows(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                   VideoCore::TextureCache& texture_cache);
    ~BbLightShadows();

    /// The game's lighting pass (its lights follow): a new frame's budget.
    void OnLightingPass();

    /// When a pixel shader's textures are bound (the draw thread): whether the draw is one of
    /// the game's light draws to shadow. Its texture descriptors (`textures`, `ids`: the first
    /// five) and buffers 0 and 1 are the draw's, `area` where it draws. The view to bind as its
    /// texture 4 instead, or none (the draw is left alone).
    vk::ImageView Prepare(u64 ps_hash, std::span<const vk::DescriptorImageInfo> textures,
                          std::span<const VideoCore::ImageId> ids, const Buffer& camera,
                          const Buffer& lights, const AmdGpu::Regs& regs, const DrawArea& area);
    /// The draw being resolved was chosen by Prepare (its shadows are queued).
    [[nodiscard]] bool HasPending() const noexcept;
    /// Its ticket, handed to the finish half with the draw.
    [[nodiscard]] u64 PreparedId() const noexcept;
    /// Before light draw `id` is recorded (the finish half, outside its render pass unless an
    /// earlier draw's batch blended it): its shadows, and those of the light draws prepared after
    /// it, in one batch.
    void RunPending(u64 id);
    /// After the scene's composite into `target`: the debug view over it (when on), the shadows
    /// the frame's passes worked out, grey and darker in shadow, red where none of them reached.
    void Show(VideoCore::ImageId target);
    /// Forgets Prepare's choice for the draw being resolved (another draw's buffers are bound).
    void ClearPending() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
