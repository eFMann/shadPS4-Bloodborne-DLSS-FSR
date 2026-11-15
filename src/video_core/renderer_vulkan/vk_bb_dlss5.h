// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss_ngx.h"

namespace Vulkan {
class Instance;
class Scheduler;

// DLSS 5 (NVIDIA's Neural Rendering model) over the upscaled scene, before the game's HUD and
// before frame generation. Windows and RTX only; the player supplies nvngx_dlssnr.dll. The
// settings are DLSS5OneClick's (OptiScaler's [DlssNr]) with the same defaults, in dlss.ini as
// dlss5_*.
struct BbDlss5Settings {
    bool enabled = false;
    bool before_upscale = true; // on the render-size image before the upscaler: faster
    float transfer_strength = 1.0f;
    float colour_strength = 1.0f;
    float white_point_scale = 4.0f;
    float max_ratio = 2.0f;
    float working_scale = 1.0f;
    u32 downscaler = 1; // 0 bilinear / area, 1 Lanczos 3
    u32 debug_view = 0; // 1 what the model sees, 2 its answer, 3 the difference x20
    u32 blending = 0;   // 0 default, 1 RenoDX's DLSS 5 add-on's (its SDR codec)
    u32 black_restore = 1; // RenoDX: 0 off, 1 dark blocks, 2 every block, 3 dark blocks smoothly
    // Read by the model when it starts: a change starts it again (after a short pause).
    u32 preset = 1; // 1 the full network, 2 and 0 (NVIDIA's default) a smaller one, 3
    u32 style = 0;  // 0 standard, 1 natural, 2 cinematic (names from community testing)
    float intensity = 1.75f;
    float local_structure = 1.0f;
    float local_tone = 1.0f;
    float skin_structure = -1.0f; // below 0: the local structure's value
    bool auto_mask = true;
    u32 passes = 1; // the model run again over its own answer, a model each: 1 to 4
};
// One dlss.ini line; false when the key isn't DLSS 5's.
bool ParseBbDlss5Setting(BbDlss5Settings& settings, std::string_view key, std::string_view value);

struct BbDlss5Status {
    bool enabled{}, running{}, hdr{};
    std::string problem; // why it isn't running, in plain words
    u32 width{}, height{}, model_width{}, model_height{};
};
BbDlss5Status BbDlss5GetStatus();

class BbDlss5 {
public:
    BbDlss5();
    ~BbDlss5();

    enum class Encoding : u32 { Srgb, Linear, ToneMapped, Hdr };
    struct Inputs {
        vk::Image image; // R16G16B16A16 float, general layout, read and written
        vk::ImageView view;
        vk::Extent2D extent;
        Encoding encoding;
        DlssNgx::Resource depth, motion; // render size, shader read-only layout
        bool depth_inverted;
        bool reset;
    };
    // On the GPU command thread after the upscaler wrote the scene: records the pass. False
    // when it did not run (turned off, or not available).
    bool Apply(const Instance& instance, Scheduler& scheduler, const BbDlss5Settings& settings,
               const Inputs& inputs);
    // After the GPU was drained.
    void Shutdown(DlssNgx* ngx);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
