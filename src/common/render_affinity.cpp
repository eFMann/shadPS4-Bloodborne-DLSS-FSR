// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <bit>
#include <mutex>
#include <vector>

#include "common/logging/log.h"
#include "common/render_affinity.h"
#include "common/types.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common::RenderAffinity {

namespace {

// Bumped on every change; 0 means "never set" (threads keep the default).
std::atomic<u32> generation{0};
std::atomic<bool> wanted{false};
thread_local u32 applied_generation = 0;

#ifdef _WIN32
// Below this many CPUs left for the render threads the rule is skipped: taking a core away from
// a small CPU would cost more than the interrupts do.
constexpr int MinCpusLeft = 6;

struct Masks {
    bool valid = false;
    DWORD_PTR process = 0;   ///< every CPU the process may use (group 0)
    DWORD_PTR off_core0 = 0; ///< the same without the logical CPUs of the core CPU 0 is on
};

Masks ComputeMasks() {
    Masks masks;
    DWORD_PTR process = 0;
    DWORD_PTR system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || process == 0) {
        return masks;
    }
    masks.process = process;
    // The logical CPUs sharing a physical core with CPU 0 (two with SMT/Hyper-Threading), asked
    // of Windows rather than assumed.
    DWORD_PTR core0 = 1;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length != 0) {
        std::vector<u8> buffer(length);
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
            for (DWORD offset = 0; offset < length;) {
                const auto* entry =
                    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() +
                                                                                     offset);
                for (WORD g = 0; g < entry->Processor.GroupCount; ++g) {
                    const GROUP_AFFINITY& group = entry->Processor.GroupMask[g];
                    if (group.Group == 0 && (group.Mask & 1) != 0) {
                        core0 = group.Mask;
                    }
                }
                offset += entry->Size;
            }
        }
    }
    masks.off_core0 = process & ~core0;
    masks.valid = std::popcount(static_cast<u64>(masks.off_core0)) >= MinCpusLeft;
    return masks;
}

const Masks& GetMasks() {
    static const Masks masks = ComputeMasks();
    return masks;
}
#endif

} // namespace

void Set(bool off_core0) {
    if (generation.load(std::memory_order_relaxed) != 0 &&
        wanted.load(std::memory_order_relaxed) == off_core0) {
        return;
    }
#ifdef _WIN32
    if (off_core0) {
        const Masks& masks = GetMasks();
        if (masks.valid) {
            LOG_INFO(Common, "Render threads off core 0: on, CPUs mask {:#x} (of {:#x})",
                     static_cast<u64>(masks.off_core0), static_cast<u64>(masks.process));
        } else {
            LOG_INFO(Common, "Render threads off core 0: skipped (too few CPUs, mask {:#x})",
                     static_cast<u64>(masks.process));
        }
    } else if (generation.load(std::memory_order_relaxed) != 0) {
        LOG_INFO(Common, "Render threads off core 0: off");
    }
#endif
    wanted.store(off_core0, std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release);
}

void Apply() {
    const u32 current = generation.load(std::memory_order_acquire);
    if (current == applied_generation) {
        return;
    }
    applied_generation = current;
#ifdef _WIN32
    const Masks& masks = GetMasks();
    if (!masks.valid) {
        return;
    }
    const bool off = wanted.load(std::memory_order_relaxed);
    SetThreadAffinityMask(GetCurrentThread(), off ? masks.off_core0 : masks.process);
#endif
}

} // namespace Common::RenderAffinity
