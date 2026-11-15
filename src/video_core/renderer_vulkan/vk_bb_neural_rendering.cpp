// SPDX-FileCopyrightText: Copyright 2026 Robert Bopko
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string_view>

#include <fmt/ranges.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_bb_library.h"
#include "video_core/renderer_vulkan/vk_bb_neural_rendering.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
// The backends, tried in order: each is a shadps4_nr.dll in its own folder next to the executable.
// Another one (NVIDIA's NR through NGX, say) is a line here.
struct BackendFiles {
    const char* folder;
    const char* model; // needed beside shadps4_nr.dll; null: nothing else
};
constexpr std::array Backends{BackendFiles{"dlssnr-amd", "dlssnr.bin"}};

std::vector<std::string> loaded_networks;

std::filesystem::path Folder(const BackendFiles& backend) {
    return BbLibrary::ExecutableDirectory() / backend.folder;
}

const BackendFiles* Installed() {
#ifdef _WIN32
    static const BackendFiles* const installed = []() -> const BackendFiles* {
        const char* setting = std::getenv("SHADPS4_NR");
        if (setting && std::string_view{setting} == "0")
            return nullptr;
        for (const auto& backend : Backends) {
            const auto folder = Folder(backend);
            if (std::filesystem::is_regular_file(folder / "shadps4_nr.dll") &&
                (!backend.model || std::filesystem::is_regular_file(folder / backend.model)))
                return &backend;
        }
        return nullptr;
    }();
    return installed;
#else
    return nullptr; // DLSSNR-AMD's Windows runtime is the one built here
#endif
}

// The network's background builds submit to the graphics queue: held like a present, which takes
// present_mutex (Keep rendering while presenting, separate present queue) and submit_mutex.
void LockQueue(void*) {
    std::lock(Scheduler::present_mutex, Scheduler::submit_mutex);
}

void UnlockQueue(void*) {
    Scheduler::submit_mutex.unlock();
    Scheduler::present_mutex.unlock();
}
} // namespace

bool BbNeuralRendering::Present() {
    return Installed() != nullptr;
}

const std::vector<std::string>& BbNeuralRendering::Networks() {
    return loaded_networks;
}

int BbNeuralRendering::NetworkIndex(std::string_view name) {
    const auto same = [name](const std::string& network) {
        return std::ranges::equal(name, network, [](char a, char b) {
            return std::toupper(static_cast<unsigned char>(a)) ==
                   std::toupper(static_cast<unsigned char>(b));
        });
    };
    const auto it = std::ranges::find_if(loaded_networks, same);
    return it == loaded_networks.end() ? -1 : int(it - loaded_networks.begin());
}

std::unique_ptr<BbNeuralRendering> BbNeuralRendering::Load() {
    const BackendFiles* backend = Installed();
    if (!backend)
        return {};
    auto nr = std::unique_ptr<BbNeuralRendering>{new BbNeuralRendering};
    nr->module = BbLibrary::Load(Folder(*backend) / "shadps4_nr.dll");
    auto& a = nr->api;
    const auto find = [&]<typename T>(T& entry, const char* name) {
        entry = BbLibrary::Find<T>(nr->module, name);
    };
    find(a.abi_version, "shadps4_nr_abi_version");
    find(a.info, "shadps4_nr_info");
    find(a.create_device, "shadps4_nr_create_device");
    find(a.init, "shadps4_nr_init");
    find(a.evaluate, "shadps4_nr_evaluate");
    find(a.status, "shadps4_nr_status");
    find(a.shutdown, "shadps4_nr_shutdown");
    if (!a.abi_version || !a.info || !a.create_device || !a.init || !a.evaluate || !a.status ||
        !a.shutdown || a.abi_version() != SHADPS4_NR_ABI_VERSION) {
        LOG_WARNING(Render_Vulkan, "[NR] {}/shadps4_nr.dll is missing or another version; NR off",
                    backend->folder);
        a = {};
        return {};
    }
    a.info(&nr->info);
    nr->info.name[sizeof nr->info.name - 1] = '\0';
    loaded_networks.clear();
    for (u32 i = 0; i < std::min<u32>(nr->info.network_count, SHADNR_MAX_NETWORKS); ++i) {
        auto& network = nr->info.networks[i];
        network[sizeof network - 1] = '\0';
        loaded_networks.emplace_back(network);
    }
    LOG_INFO(Render_Vulkan, "[NR] {} loaded from {}/shadps4_nr.dll, networks: {}", nr->info.name,
             backend->folder, fmt::join(loaded_networks, ", "));
    return nr;
}

BbNeuralRendering::~BbNeuralRendering() {
    if (api.shutdown)
        api.shutdown();
    // Once the device was made through it, the model's detached threads may still run its code:
    // the DLL stays loaded until the process ends.
    if (!device_made)
        BbLibrary::Free(module);
}

vk::ResultValue<vk::UniqueDevice> BbNeuralRendering::CreateDevice(
    vk::PhysicalDevice physical, const vk::DeviceCreateInfo& info) {
    VkDevice device{};
    u32 added{};
    const auto result = static_cast<vk::Result>(api.create_device(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDevice, static_cast<VkPhysicalDevice>(physical),
        &static_cast<const VkDeviceCreateInfo&>(info), nullptr, &device, &added));
    device_made = true;
    LOG_INFO(Render_Vulkan, "[NR] Device created {} the network's features",
             added ? "with" : "without");
    return {result, vk::UniqueDevice{device}};
}

bool BbNeuralRendering::Init(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device,
                             u32 queue_family) {
    const auto result =
        api.init(static_cast<VkInstance>(instance), static_cast<VkPhysicalDevice>(physical),
                 static_cast<VkDevice>(device), queue_family, LockQueue, UnlockQueue, nullptr);
    const bool ready = result == 0;
    if (ready)
        LOG_INFO(Render_Vulkan, "[NR] Ready on queue family {}", queue_family);
    else
        LOG_WARNING(Render_Vulkan, "[NR] {} did not start (see its log); NR off", info.name);
    return ready;
}

bool BbNeuralRendering::Evaluate(vk::CommandBuffer command, const ShadNrFrame& frame,
                                 const ShadNrSettings& settings) {
    return api.evaluate(static_cast<VkCommandBuffer>(command), &frame, &settings) == 0;
}

ShadNrStatus BbNeuralRendering::Status() const {
    ShadNrStatus status{};
    api.status(&status);
    return status;
}

} // namespace Vulkan
