// SPDX-FileCopyrightText: Copyright 2026 Robert Bopko
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include "../../../dlssnr_addon/shadps4_nr.h"

namespace Vulkan {

// Optional DLSS 5 neural rendering: the first installed backend of the list in the .cpp, a
// shadps4_nr.dll (dlssnr_addon/shadps4_nr.h) in its own folder next to the executable, today
// dlssnr-amd with the model mochizuki0323's DLSSNR-AMD installer extracts. It runs the network on
// shadPS4's device, so the device is created through it; with no backend installed nothing about
// the device changes.
class BbNeuralRendering {
public:
    // A backend's files are present and SHADPS4_NR is not "0"; Windows only.
    static bool Present();
    // CC_SMALL_FP8 can be offered (dlssnr-amd: dlssnr-small.bin is installed).
    static bool SmallModelPresent();
    // Null when the backend cannot be loaded or speaks another ABI version.
    static std::unique_ptr<BbNeuralRendering> Load();
    ~BbNeuralRendering();

    const char* Name() const {
        return info.name;
    }
    // upscaler: a SHADNR_BEFORE_* bit.
    bool RunsBefore(u32 upscaler) const {
        return (info.upscalers & upscaler) != 0;
    }

    vk::Result CreateDevice(vk::PhysicalDevice physical, const vk::DeviceCreateInfo& info,
                            vk::Device& out);
    bool Init(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device,
              u32 queue_family);
    // False: the network failed; the caller stops using the add-on.
    bool Evaluate(vk::CommandBuffer command, const ShadNrFrame& frame,
                  const ShadNrSettings& settings);
    ShadNrStatus Status() const;

private:
    BbNeuralRendering() = default;
    struct Api;
    void* module{};
    std::unique_ptr<Api> api;
    ShadNrInfo info{};
    bool device_made{};
};

} // namespace Vulkan
