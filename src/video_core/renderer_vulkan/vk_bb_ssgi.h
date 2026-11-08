// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;

/// Screen-space global illumination (SSGI): light bouncing between the surfaces on screen.
/// Bloodborne's only indirect light is one flat probe per object (G-buffer target 4: a constant
/// plus normal times a direction), so light never spills from a lit wall onto the floor next to
/// it. Its lighting pass (pixel shader 3fe9e964) multiplies that diffuse light by the AO into
/// its output 0, the lamps and fires add theirs to it after, pixel shader 8650418f then
/// multiplies it by the base colour, and the scene's composite (32e47788) adds the specular and
/// the fog.
/// After the lighting pass, the bounce is gathered with a visibility bitmask from the light the
/// surfaces gave off last frame (8650418f's result, kept at half size and reprojected), and added
/// to the lighting's output 0 times the game's AO: the game then applies the base colour and the
/// fog itself, and the specular and reflections are left alone. The light kept leaves out the
/// game's flat ambient light (its output 0 as the lighting pass wrote it): bounced again it only
/// lifted everything evenly. It holds the last frame's bounce, so further bounces build up.
class BbSsgi {
public:
    /// The game's lighting pass (as BbSsr::LightingShader).
    static constexpr u64 LightingShader = 0x3fe9e964;
    /// Its diffuse light times the base colour (texture 0 times texture 1).
    static constexpr u64 DiffuseAlbedoShader = 0x8650418f;
    /// The AO brought to the scene size for the lighting (red times green is the occlusion).
    static constexpr u64 AoCompositeShader = 0xa291c498;
    /// The scene's composite: diffuse, specular and the fog (the debug view goes over it).
    static constexpr u64 SceneCompositeShader = 0x32e47788;

    [[nodiscard]] static bool UsesShader(u64 ps_hash) noexcept {
        return ps_hash == LightingShader || ps_hash == DiffuseAlbedoShader ||
               ps_hash == AoCompositeShader || ps_hash == SceneCompositeShader;
    }

    BbSsgi(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
           VideoCore::TextureCache& texture_cache);
    ~BbSsgi();

    /// A G-buffer draw (5+ color targets) into `depth`, with `normals` its target 0 and the signs
    /// of its viewport's scales: the scene is the largest such pass of the frame.
    void OnGBufferDraw(VideoCore::ImageId depth, VideoCore::ImageId normals, float x_sign,
                       float y_sign);
    /// Not a G-buffer draw (its constants are not the scene camera's).
    void OnOtherDraw() noexcept {
        gbuffer_draw = false;
    }
    /// Constant data bound to a draw: the first scene constants of the G-buffer pass are its
    /// camera.
    void ObserveSceneConstants(const void* data, u64 size);

    /// After the draw that brings the AO to the scene size, into `target`.
    void OnAoComposite(VideoCore::ImageId target);
    /// After the lighting draw, whose output 0 (the diffuse light) is `diffuse`, drawn as
    /// `view_format`: the bounce is added to it.
    void OnLighting(VideoCore::ImageId diffuse, vk::Format view_format);
    /// After the draw that multiplies the diffuse light by the base colour into `target`.
    void OnDiffuseAlbedo(VideoCore::ImageId target);
    /// After the scene's composite into `target`, whose texture 5 is `self_lit` (the light of
    /// glowing surfaces: the G-buffer's emissive target and self-lit glass): the light the
    /// surfaces give off is kept for the next frame's bounce, and a debug view is written over
    /// the scene (when on).
    void OnSceneComposite(VideoCore::ImageId target, VideoCore::ImageId self_lit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    bool gbuffer_draw{};
};

} // namespace Vulkan
