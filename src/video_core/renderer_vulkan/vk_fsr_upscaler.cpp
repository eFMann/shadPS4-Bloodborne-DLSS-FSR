// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_bb_temporal_dlss.h"
#include "video_core/renderer_vulkan/vk_fsr_upscaler.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#ifndef _WIN32
// ffx_api.h marks its declarations __declspec(dllexport).
#define __declspec(x)
#endif
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#ifndef _WIN32
#undef __declspec
#endif
#include "video_core/renderer_vulkan/vk_bb_library.h"

namespace Vulkan {

namespace {
constexpr char FsrName[] = BB_LIBRARY_NAME("amd_fidelityfx_vk.dll", "libamd_fidelityfx_vk.so");

std::filesystem::path AmdFsrDirectory() {
    return BbLibrary::ExecutableDirectory() / "amd_fsr";
}

std::filesystem::path LoaderPath(FsrUpscaler::Runtime runtime) {
    return runtime == FsrUpscaler::Runtime::AmdFsr
               ? AmdFsrDirectory() / "amd_fidelityfx_loader_vk.dll"
               : BbLibrary::ExecutableDirectory() / FsrName;
}
} // namespace

bool FsrUpscaler::Present(Runtime runtime) {
    static const bool disabled = [] {
        const char* setting = std::getenv("SHADPS4_FSR");
        return setting && std::string_view{setting} == "0";
    }();
    static const bool fsr31 = std::filesystem::is_regular_file(LoaderPath(Runtime::Fsr31));
#ifdef _WIN32
    static const bool amd_fsr = std::filesystem::is_regular_file(LoaderPath(Runtime::AmdFsr));
#else
    constexpr bool amd_fsr = false; // AMD ships the FSR SDK 2.x runtime for Windows only
#endif
    return !disabled && (runtime == Runtime::AmdFsr ? amd_fsr : fsr31);
}

#if defined(_WIN32) || defined(__linux__)

namespace {
void FfxMessage(uint32_t type, const wchar_t* message) {
    const auto text = std::filesystem::path{message}.string();
    if (type == FFX_API_MESSAGE_TYPE_ERROR)
        LOG_ERROR(Render_Vulkan, "[FSR] {}", text);
    else
        LOG_WARNING(Render_Vulkan, "[FSR] {}", text);
}

VKAPI_ATTR VkResult VKAPI_CALL NoObjectName(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) {
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL NoLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
VKAPI_ATTR void VKAPI_CALL NoLabelEnd(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL NoMarker(VkCommandBuffer, VkPipelineStageFlagBits, VkBuffer,
                                    VkDeviceSize, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL NoMarker2(VkCommandBuffer, VkPipelineStageFlags2, VkBuffer, VkDeviceSize,
                                     uint32_t) {}

// The FidelityFX backend looks functions up by extension name and calls some without checking.
// Fall back to the core name (e.g. vkGetBufferMemoryRequirements2KHR -> ...2), and give debug
// label functions no-op stand-ins when debug utils are not enabled. On AMD cards it also checks
// for the buffer marker extension on the GPU rather than on the device shadPS4 created, so the
// marker functions get no-op stand-ins as well.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProcAddr(VkDevice device, const char* name) {
    const auto get = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    if (auto function = get(device, name))
        return function;
    const std::string_view view{name};
    for (const std::string_view suffix : {"KHR", "EXT"}) {
        if (view.ends_with(suffix)) {
            const std::string core{view.substr(0, view.size() - suffix.size())};
            if (auto function = get(device, core.c_str())) {
                LOG_INFO(Render_Vulkan, "[FSR] {} -> {}", name, core);
                return function;
            }
        }
    }
    if (view == "vkSetDebugUtilsObjectNameEXT")
        return reinterpret_cast<PFN_vkVoidFunction>(&NoObjectName);
    if (view == "vkCmdBeginDebugUtilsLabelEXT" || view == "vkCmdInsertDebugUtilsLabelEXT")
        return reinterpret_cast<PFN_vkVoidFunction>(&NoLabel);
    if (view == "vkCmdEndDebugUtilsLabelEXT")
        return reinterpret_cast<PFN_vkVoidFunction>(&NoLabelEnd);
    if (view == "vkCmdWriteBufferMarkerAMD")
        return reinterpret_cast<PFN_vkVoidFunction>(&NoMarker);
    if (view == "vkCmdWriteBufferMarker2AMD")
        return reinterpret_cast<PFN_vkVoidFunction>(&NoMarker2);
    LOG_WARNING(Render_Vulkan, "[FSR] Vulkan function {} is not available", name);
    return nullptr;
}

uint32_t SurfaceFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
        return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case vk::Format::eR32Sfloat:
        return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case vk::Format::eR16G16Sfloat:
        return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case vk::Format::eR16G16B16A16Sfloat:
        return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    default:
        return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxApiResource ToFfx(const DlssNgx::Resource& resource, bool writable) {
    FfxApiResource result{};
    result.resource = reinterpret_cast<void*>(static_cast<VkImage>(resource.image));
    result.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    result.description.format = SurfaceFormat(resource.format);
    result.description.width = resource.extent.width;
    result.description.height = resource.extent.height;
    result.description.depth = 1;
    result.description.mipCount = 1;
    result.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    result.description.usage =
        writable ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    // Inputs arrive in SHADER_READ_ONLY_OPTIMAL, the output in GENERAL.
    result.state =
        writable ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return result;
}
// FSR SDK 2.x renumbered the Vulkan backend description to
// FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_VK, 0x03) and rejects the 1.x number. The struct
// layout is unchanged, so the 1.1.4 headers serve both runtimes.
constexpr ffxStructType_t AmdFsrBackendVkType = 0x01000003u;
// SDK 2.1 and later want the upscaler API version the game was built against chained to the
// context description (ffxCreateContextDescUpscaleVersion, absent from the 1.1.4 headers):
// FFX_UPSCALER_VERSION of the pinned SDK 2.4.0, upscaler 4.1.2.
struct AmdFsrUpscaleVersionDesc {
    ffxCreateContextDescHeader header;
    uint32_t version;
};
constexpr ffxStructType_t AmdFsrUpscaleVersionType = 0x0001000bu;
constexpr uint32_t AmdFsrUpscalerVersion = (4u << 22) | (1u << 12) | 2u;
// FSR SDK 2.x's VkDeviceContext (ffx_api_vk.h), the device a version query is asked about.
struct AmdFsrDeviceContext {
    VkDevice device;
    VkPhysicalDevice physical_device;
    PFN_vkGetDeviceProcAddr device_proc_addr;
};
constexpr u32 AmdVendorId = 0x1002;

// The newest FSR 4 version AMD's runtime offers on this GPU; empty when it offers none. The query
// is asked about this device, the way the SDK's "Version selection" lists versions for a game's
// options.
std::string NewestFsr4(PfnFfxQuery query, vk::Device device, vk::PhysicalDevice physical) {
    AmdFsrDeviceContext context{device, physical, DeviceProcAddr};
    uint64_t count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &context;
    versions.outputCount = &count;
    if (query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0)
        return {};
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (query(nullptr, &versions.header) != FFX_API_RETURN_OK)
        return {};
    names.resize(std::min<size_t>(count, names.size()));
    std::string offered, newest;
    std::array<int, 3> newest_number{};
    for (const char* name : names) {
        if (!name)
            continue;
        const std::string_view text{name};
        offered += offered.empty() ? std::string{text} : fmt::format(", {}", text);
        std::array<int, 3> number{};
        size_t at = text.find_first_of("0123456789");
        for (int& part : number) {
            if (at == std::string_view::npos || at >= text.size())
                break;
            std::from_chars(text.data() + at, text.data() + text.size(), part);
            at = text.find('.', at);
            at = at == std::string_view::npos ? at : at + 1;
        }
        if (number[0] >= 4 && number > newest_number) {
            newest_number = number;
            newest = fmt::format("{}.{}.{}", number[0], number[1], number[2]);
        }
    }
    LOG_INFO(Render_Vulkan, "[FSR] AMD FSR runtime's upscalers on this GPU: {}",
             offered.empty() ? "none" : offered);
    return newest;
}
} // namespace

struct FsrUpscaler::Impl {
    Runtime runtime{};
    std::string version; // AMD FSR: the FSR 4 version it runs on this GPU
    void* module{};
    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
    vk::Device device;
    vk::PhysicalDevice physical;
    ffxContext context{};
    std::optional<ContextDesc> desc;
};

FsrUpscaler::FsrUpscaler() : impl{std::make_unique<Impl>()} {}

void* FsrUpscaler::FfxDeviceProcAddr() {
    return reinterpret_cast<void*>(&DeviceProcAddr);
}

std::unique_ptr<FsrUpscaler> FsrUpscaler::Create(const Instance& instance, Runtime runtime) {
    if (!Present(runtime))
        return {};
    auto fsr = std::unique_ptr<FsrUpscaler>{new FsrUpscaler};
    auto& s = *fsr->impl;
    s.runtime = runtime;
    const auto path = LoaderPath(runtime);
    const auto name = path.filename().string();
    s.module = BbLibrary::Load(path);
    if (!s.module) {
        LOG_WARNING(Render_Vulkan, "[FSR] {} could not be loaded", name);
        return {};
    }
    s.create = BbLibrary::Find<PfnFfxCreateContext>(s.module, "ffxCreateContext");
    s.destroy = BbLibrary::Find<PfnFfxDestroyContext>(s.module, "ffxDestroyContext");
    s.query = BbLibrary::Find<PfnFfxQuery>(s.module, "ffxQuery");
    s.dispatch = BbLibrary::Find<PfnFfxDispatch>(s.module, "ffxDispatch");
    if (!s.create || !s.destroy || !s.query || !s.dispatch) {
        LOG_WARNING(Render_Vulkan, "[FSR] {} is missing entry points", name);
        BbLibrary::Free(s.module);
        s.module = {};
        return {};
    }
    s.device = instance.GetDevice();
    s.physical = instance.GetPhysicalDevice();
    if (runtime == Runtime::AmdFsr) {
        // FSR 4.1 runs on some Radeon and Ryzen GPUs only; elsewhere it isn't offered at all.
        if (s.physical.getProperties().vendorID == AmdVendorId)
            s.version = NewestFsr4(s.query, s.device, s.physical);
        if (s.version.empty()) {
            LOG_INFO(Render_Vulkan, "[FSR] AMD FSR 4 does not run on this GPU; not offered");
            BbLibrary::Free(s.module);
            s.module = {};
            return {};
        }
    }
    BbTemporalDlssReportGpu(s.physical.getProperties().deviceName.data());
    LOG_INFO(Render_Vulkan, "[FSR] Ready ({}{})", name,
             s.version.empty() ? "" : fmt::format(", FSR {}", s.version));
    return fsr;
}

const std::string& FsrUpscaler::Version() const {
    return impl->version;
}

FsrUpscaler::~FsrUpscaler() {
    DestroyContextAfterGpuDrain();
    BbLibrary::Free(impl->module);
}

bool FsrUpscaler::HasContext(const ContextDesc& desc) const {
    return impl->desc && *impl->desc == desc;
}

bool FsrUpscaler::CreateContext(const ContextDesc& desc) {
    DestroyContextAfterGpuDrain();
    ffxCreateBackendVKDesc backend{};
    backend.header.type = impl->runtime == Runtime::AmdFsr
                              ? AmdFsrBackendVkType
                              : FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = impl->device;
    backend.vkPhysicalDevice = impl->physical;
    backend.vkDeviceProcAddr = DeviceProcAddr;
    AmdFsrUpscaleVersionDesc version_desc{};
    version_desc.header.type = AmdFsrUpscaleVersionType;
    version_desc.header.pNext = &backend.header;
    version_desc.version = AmdFsrUpscalerVersion;
    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = impl->runtime == Runtime::AmdFsr ? &version_desc.header : &backend.header;
    create.flags = (desc.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
                                   (desc.auto_exposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0u)
                             : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) |
                   (desc.depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u);
    create.maxRenderSize = {desc.render.width, desc.render.height};
    create.maxUpscaleSize = {desc.output.width, desc.output.height};
    create.fpMessage = FfxMessage;
    const auto result = impl->create(&impl->context, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        impl->context = {};
        LOG_ERROR(Render_Vulkan, "[FSR] Context creation failed ({})", result);
        return false;
    }
    impl->desc = desc;
    ffxQueryGetProviderVersion version{};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    impl->query(&impl->context, &version.header);
    LOG_INFO(Render_Vulkan, "[FSR] Context {}x{} -> {}x{} ({})", desc.render.width,
             desc.render.height, desc.output.width, desc.output.height,
             version.versionName ? version.versionName : "unknown version");
    return true;
}

void FsrUpscaler::DestroyContextAfterGpuDrain() {
    if (impl->context)
        impl->destroy(&impl->context, nullptr);
    impl->context = {};
    impl->desc.reset();
}

bool FsrUpscaler::Evaluate(vk::CommandBuffer command, const DlssNgx::Resource& color,
                           const DlssNgx::Resource& depth, const DlssNgx::Resource& motion,
                           const DlssNgx::Resource& output, const DlssNgx::EvalDesc& eval,
                           const Camera& camera) {
    if (!impl->context || !impl->desc)
        return false;
    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = static_cast<VkCommandBuffer>(command);
    dispatch.color = ToFfx(color, false);
    dispatch.depth = ToFfx(depth, false);
    dispatch.motionVectors = ToFfx(motion, false);
    dispatch.output = ToFfx(output, true);
    dispatch.jitterOffset = {eval.jitter_x, eval.jitter_y};
    dispatch.motionVectorScale = {1.0f, 1.0f}; // already in render pixels
    dispatch.renderSize = {color.extent.width, color.extent.height};
    dispatch.upscaleSize = {output.extent.width, output.extent.height};
    dispatch.enableSharpening = false; // the composite pass sharpens for every upscaler
    dispatch.frameTimeDelta = eval.frame_ms;
    dispatch.preExposure = 1.0f;
    dispatch.reset = eval.reset;
    dispatch.cameraNear = camera.near_plane;
    dispatch.cameraFar = camera.far_plane;
    dispatch.cameraFovAngleVertical = camera.fov_y;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = impl->desc->hdr ? 0u : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const auto result = impl->dispatch(&impl->context, &dispatch.header);
    if (result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "[FSR] Dispatch failed ({})", result);
        return false;
    }
    return true;
}

#else

struct FsrUpscaler::Impl {};
FsrUpscaler::FsrUpscaler() = default;
FsrUpscaler::~FsrUpscaler() = default;
std::unique_ptr<FsrUpscaler> FsrUpscaler::Create(const Instance&, Runtime) {
    return {};
}
const std::string& FsrUpscaler::Version() const {
    static const std::string none;
    return none;
}
bool FsrUpscaler::HasContext(const ContextDesc&) const {
    return false;
}
bool FsrUpscaler::CreateContext(const ContextDesc&) {
    return false;
}
void FsrUpscaler::DestroyContextAfterGpuDrain() {}
void* FsrUpscaler::FfxDeviceProcAddr() {
    return nullptr;
}
bool FsrUpscaler::Evaluate(vk::CommandBuffer, const DlssNgx::Resource&, const DlssNgx::Resource&,
                           const DlssNgx::Resource&, const DlssNgx::Resource&,
                           const DlssNgx::EvalDesc&, const Camera&) {
    return false;
}

#endif

} // namespace Vulkan
