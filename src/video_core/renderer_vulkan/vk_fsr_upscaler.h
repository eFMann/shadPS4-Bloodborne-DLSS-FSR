// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss_ngx.h"

namespace Vulkan {

class Instance;

// AMD FSR upscaling through one of two AMD runtimes, see Runtime. Without the runtime's DLLs
// rendering stays unchanged. Callers own synchronization and must drain recorded work before
// destroying a context.
class FsrUpscaler {
public:
    enum class Runtime {
        // amd_fidelityfx_vk.dll (libamd_fidelityfx_vk.so) next to the executable: FidelityFX
        // SDK 1.1.4, FSR 3.1 on any GPU.
        Fsr31,
        // amd_fsr\amd_fidelityfx_loader_vk.dll with amd_fidelityfx_upscaler_vk.dll beside it:
        // AMD FSR SDK 2.4 or later, FSR 4.1 on GPUs that support it. Windows only.
        AmdFsr,
    };
    // True when the runtime's loader DLL is present and FSR is not disabled.
    static bool Present(Runtime runtime = Runtime::Fsr31);
    // Null when the DLL cannot be loaded, and for AmdFsr when it offers no FSR 4 on this GPU.
    static std::unique_ptr<FsrUpscaler> Create(const Instance& instance,
                                               Runtime runtime = Runtime::Fsr31);
    // AmdFsr: the FSR 4 version it runs on this GPU, e.g. "4.1.2". Empty for Fsr31.
    const std::string& Version() const;
    // The vkGetDeviceProcAddr the FidelityFX backend gets (PFN_vkGetDeviceProcAddr), with
    // stand-ins for functions it calls that the device may lack.
    static void* FfxDeviceProcAddr();
    ~FsrUpscaler();

    struct ContextDesc {
        vk::Extent2D render, output;
        bool depth_inverted;
        bool hdr;           // linear HDR colour instead of sRGB-encoded LDR
        bool auto_exposure; // with hdr: exposure computed by FSR instead of 1
        bool operator==(const ContextDesc&) const = default;
    };
    bool HasContext(const ContextDesc& desc) const;
    // Destroys an existing context first; the caller must have drained the GPU if one exists.
    bool CreateContext(const ContextDesc& desc);
    void DestroyContextAfterGpuDrain();

    struct Camera {
        float near_plane, far_plane, fov_y; // view-space units and radians
    };
    // Same conventions as DLSS: jitter and motion vectors in render pixels, motion vectors from
    // the current to the previous frame.
    bool Evaluate(vk::CommandBuffer command, const DlssNgx::Resource& color,
                  const DlssNgx::Resource& depth, const DlssNgx::Resource& motion,
                  const DlssNgx::Resource& output, const DlssNgx::EvalDesc& eval,
                  const Camera& camera);

private:
    FsrUpscaler();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
