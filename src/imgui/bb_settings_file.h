// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/vk_bb_temporal_dlss.h"

namespace ImGui {

/// The upscaling settings file the renderer polls (key=value lines), edited by F1 and F3 alike:
/// every change is written at once, so both show the same.
class BbSettingsFile {
public:
    void Load() {
        lines.clear();
        std::ifstream file{Path()};
        for (std::string line; std::getline(file, line);)
            lines.push_back(line);
    }
    std::string Get(const std::string& key, const std::string& fallback) const {
        for (const auto& line : lines)
            if (line.rfind(key + "=", 0) == 0)
                return line.substr(key.size() + 1);
        return fallback;
    }
    void Set(const std::string& key, const std::string& value) {
        Load(); // keep edits made to the file while the panel was open
        for (auto& line : lines)
            if (line.rfind(key + "=", 0) == 0) {
                line = key + "=" + value;
                return Save();
            }
        lines.push_back(key + "=" + value);
        Save();
    }

private:
    static std::filesystem::path Path() {
        return Vulkan::BbDlssSettingsPath();
    }
    void Save() const {
        std::ofstream file{Path(), std::ios::trunc};
        for (const auto& line : lines)
            file << line << '\n';
    }
    std::vector<std::string> lines;
};

// Anisotropic filtering's samples (0: the game's own, 2, 4, 8, 16) as the menus' list index 0..4,
// and back.
inline int BbAnisotropyIndex(int samples) {
    return samples >= 16 ? 4 : samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
}
inline int BbAnisotropySamples(int index) {
    return index <= 0 ? 0 : 1 << std::min(index, 4);
}

} // namespace ImGui
