// SPDX-FileCopyrightText: Copyright 2026 Robert Bopko
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <string_view>

namespace Vulkan {

// dlss.ini's neural rendering keys (nr, nr_network, nr_scale, nr_intensity).
struct BbNrSettings {
    bool enabled = false;
    int network = -1; // -1 automatic, 0 CC_SMALL_FP8, 1 CC_SILVER_AARDWOLD
    float scale = 0.67f;
    float intensity = 1.0f;
};

// A plain decimal ("0.67", "2"); false for anything else.
constexpr bool ParseBbNrDecimal(std::string_view text, float& out) {
    float value = 0.f, unit = 1.f;
    bool digits = false, fraction = false;
    for (const char c : text) {
        if (c == '.' && !fraction) {
            fraction = true;
        } else if (c >= '0' && c <= '9') {
            digits = true;
            if (fraction) {
                unit /= 10.f;
                value += float(c - '0') * unit;
            } else {
                value = value * 10.f + float(c - '0');
            }
        } else {
            return false;
        }
    }
    if (digits)
        out = value;
    return digits;
}

// On/off as std::atoi reads it, like dlss.ini's other switches: leading spaces, then a whole
// number; "off", "" or "0" are off.
constexpr bool ParseBbNrSwitch(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    if (!text.empty() && (text.front() == '+' || text.front() == '-'))
        text.remove_prefix(1);
    for (const char c : text) {
        if (c < '0' || c > '9')
            return false;
        if (c != '0')
            return true;
    }
    return false;
}

// True when key is one of the neural rendering keys; values out of range are clamped, unreadable
// ones keep the current value.
constexpr bool ParseBbNrSetting(std::string_view key, std::string_view value, BbNrSettings& out) {
    float number{};
    if (key == "nr") {
        out.enabled = ParseBbNrSwitch(value);
    } else if (key == "nr_network") {
        out.network = value.starts_with("small") ? 0 : value.starts_with("silver") ? 1 : -1;
    } else if (key == "nr_scale") {
        if (ParseBbNrDecimal(value, number))
            out.scale = std::clamp(number, 0.33f, 1.f);
    } else if (key == "nr_intensity") {
        if (ParseBbNrDecimal(value, number))
            out.intensity = std::clamp(number, 0.f, 2.f);
    } else {
        return false;
    }
    return true;
}

// The network to run: CC_SMALL_FP8 only when its model is installed.
constexpr int BbNrNetwork(const BbNrSettings& settings, bool small_present) {
    return settings.network == 1 || !small_present ? 1 : 0;
}

static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr", "1", s) && s.enabled;
}());
static_assert([] {
    BbNrSettings s{.enabled = true};
    return ParseBbNrSetting("nr", "off", s) && !s.enabled;
}());
static_assert([] {
    BbNrSettings s{.enabled = true};
    return ParseBbNrSetting("nr", " 0", s) && !s.enabled;
}());
static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr", " 1", s) && s.enabled;
}());
static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr_scale", "0.1", s) && s.scale == 0.33f;
}());
static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr_scale", "1.5", s) && s.scale == 1.0f;
}());
static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr_scale", "0.50", s) && s.scale == 0.5f;
}());
static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr_scale", "abc", s) && s.scale == 0.67f;
}());
static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr_intensity", "3", s) && s.intensity == 2.0f;
}());
static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr_network", "small", s) && s.network == 0;
}());
static_assert([] {
    BbNrSettings s;
    return ParseBbNrSetting("nr_network", "silver", s) && s.network == 1;
}());
static_assert([] {
    BbNrSettings s;
    return !ParseBbNrSetting("upscaler", "amdfsr", s);
}());
static_assert(BbNrNetwork(BbNrSettings{}, true) == 0 && BbNrNetwork(BbNrSettings{}, false) == 1);
static_assert(BbNrNetwork(BbNrSettings{.network = 0}, false) == 1); // small asked, not installed

} // namespace Vulkan
