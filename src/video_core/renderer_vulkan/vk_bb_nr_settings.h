// SPDX-FileCopyrightText: Copyright 2026 Robert Bopko
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "video_core/renderer_vulkan/vk_retro_hdr.h"

namespace Vulkan {

constexpr float BbNrScaleMin = 0.33f;
constexpr float BbNrScaleMax = 1.0f;
constexpr float BbNrIntensityMax = 2.0f;

// dlss.ini's neural rendering keys (nr, nr_network, nr_scale, nr_intensity), read the same way by
// the renderer and by F1.
struct BbNrSettings {
    bool enabled = false;
    std::string network = "auto"; // a name of BbNeuralRendering::Networks(), or automatic
    float scale = 0.67f;
    float intensity = 1.0f;
};

// True when key is one of nr_network, nr_scale and nr_intensity (nr is read with dlss.ini's other
// switches); values out of range are clamped, unreadable ones keep the current value.
inline bool ParseBbNrSetting(std::string_view key, std::string_view value, BbNrSettings& out) {
    if (key == "nr_network") {
        const auto end = value.find_last_not_of(" \t\r");
        out.network = value.substr(0, end == std::string_view::npos ? 0 : end + 1);
    } else if (key == "nr_scale") {
        out.scale = std::clamp(RetroHdr::ParseFloat(value, out.scale), BbNrScaleMin, BbNrScaleMax);
    } else if (key == "nr_intensity") {
        out.intensity =
            std::clamp(RetroHdr::ParseFloat(value, out.intensity), 0.0f, BbNrIntensityMax);
    } else {
        return false;
    }
    return true;
}

} // namespace Vulkan
