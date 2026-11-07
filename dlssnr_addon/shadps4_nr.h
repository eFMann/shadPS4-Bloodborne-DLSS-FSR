// SPDX-FileCopyrightText: Copyright 2026 Robert Bopko
// SPDX-License-Identifier: MIT

// The C interface of a DLSS 5 neural rendering backend: a shadps4_nr.dll in the backend's own
// folder next to shadPS4.exe, run in place on the render-size colour just before the upscaler on
// shadPS4's own Vulkan device. The one built here is dlssnr-amd/shadps4_nr.dll, mochizuki0323's
// DLSSNR-AMD network (gitlab.com/mochizuki0323/DLSSNR-AMD, MIT); another backend (NVIDIA's own
// NR through NGX, say) exports the same functions from its own folder, which
// vk_bb_neural_rendering.cpp lists. Backends may be built with another compiler than shadPS4 (this
// one with mingw, shadPS4 with MSVC), so everything here is plain C with fixed-size fields; the
// static_asserts below fail one of the two builds if the layouts ever differ.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef SHADPS4_NR_BUILD
#define SHADPS4_NR_API __declspec(dllexport)
#else
#define SHADPS4_NR_API
#endif

#define SHADPS4_NR_ABI_VERSION 2u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShadNrImage {
    uint64_t image; // VkImage
    uint32_t format, width, height, layout, usage;
} ShadNrImage;

typedef struct ShadNrFrame {
    ShadNrImage color;  // read and written in place, VK_IMAGE_LAYOUT_GENERAL, STORAGE usage
    ShadNrImage depth;  // SHADER_READ_ONLY_OPTIMAL; image 0 = none
    ShadNrImage motion; // SHADER_READ_ONLY_OPTIMAL; image 0 = none
    uint32_t width, height;               // render size: the colour's working extent
    uint32_t output_width, output_height; // the upscaler's output size
    float mv_scale_x, mv_scale_y;
    uint32_t depth_inverted, reset;
} ShadNrFrame;

typedef struct ShadNrSettings {
    int32_t network; // 0 CC_SMALL_FP8, 1 CC_SILVER_AARDWOLD
    float scale;     // the network's resolution, 0.33 to 1 of the frame
    float intensity; // 0 to 2
} ShadNrSettings;

enum { SHADNR_IDLE = 0, SHADNR_BUILDING = 1, SHADNR_RUNNING = 2, SHADNR_FAILED = 3 };

// The upscalers a backend runs in front of (ShadNrInfo.upscalers).
enum {
    SHADNR_BEFORE_DLSS = 1,
    SHADNR_BEFORE_FSR31 = 2,
    SHADNR_BEFORE_FSR4 = 4,
    SHADNR_BEFORE_AMD_FSR = 8,
};

typedef struct ShadNrInfo {
    char name[32];      // for the log, e.g. "DLSSNR-AMD"
    uint32_t upscalers; // SHADNR_BEFORE_* bits
} ShadNrInfo;

typedef struct ShadNrStatus {
    uint32_t state;
    float network_ms, gpu_ms;
    uint32_t width, height; // the running network's extent (0 x 0 before one runs)
    char network[32];
    char message[224];
} ShadNrStatus;

typedef void (*ShadNrQueueLock)(void* context);

SHADPS4_NR_API uint32_t shadps4_nr_abi_version(void);
SHADPS4_NR_API void shadps4_nr_info(ShadNrInfo* out);
// vkCreateDevice with the network's extensions and features added; made as asked when that fails
// (*features_added then 0).
SHADPS4_NR_API VkResult shadps4_nr_create_device(PFN_vkCreateDevice create, VkPhysicalDevice physical,
                                                 const VkDeviceCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator,
                                                 VkDevice* out, uint32_t* features_added);
// The network's background builds submit to queue 0 of queue_family between lock and unlock.
// 0 = ready.
SHADPS4_NR_API int32_t shadps4_nr_init(VkInstance instance, VkPhysicalDevice physical,
                                       VkDevice device, uint32_t queue_family, ShadNrQueueLock lock,
                                       ShadNrQueueLock unlock, void* context);
// 0 = ran, or passed the frame through unchanged while the network builds; < 0 = failed. Called on
// shadPS4's render thread before FSR and on its recording thread before DLSS, never both at once;
// cmd is the frame's command buffer, recorded in order with the upscaler that follows.
SHADPS4_NR_API int32_t shadps4_nr_evaluate(VkCommandBuffer cmd, const ShadNrFrame* frame,
                                           const ShadNrSettings* settings);
SHADPS4_NR_API void shadps4_nr_status(ShadNrStatus* out);
// Waits for the model's queue work to finish and drops the session; call before the device is
// destroyed.
SHADPS4_NR_API void shadps4_nr_shutdown(void);

#ifdef __cplusplus
}
static_assert(sizeof(ShadNrImage) == 32, "ShadNrImage layout");
static_assert(sizeof(ShadNrFrame) == 128, "ShadNrFrame layout");
static_assert(sizeof(ShadNrSettings) == 12, "ShadNrSettings layout");
static_assert(sizeof(ShadNrStatus) == 276, "ShadNrStatus layout");
static_assert(sizeof(ShadNrInfo) == 36, "ShadNrInfo layout");
#endif
