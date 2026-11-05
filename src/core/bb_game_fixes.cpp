// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstring>
#include <string_view>

#include "common/crash_crumbs.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/types.h"
#include "core/bb_game_fixes.h"
#include "core/emulator_settings.h"

namespace Core::BbGameFixes {
namespace {

// Offsets in the game's image (patch-file address - 0x400000).
u8* At(u64 offset) {
    return reinterpret_cast<u8*>(MemoryPatcher::g_eboot_address) + offset;
}

bool Fits(u64 offset, u64 size) {
    return offset + size <= MemoryPatcher::g_eboot_image_size;
}

// Save-load crash ("Unimplemented PM4 type 0"): while an effect sets up, the
// game's main thread clears three small history targets (0xebb960) through the same command
// buffer its render thread is filling at that moment. The two threads advance one cursor, and the
// buffer gets holes of zeros the command processor cannot decode. These three clears are done here
// instead, by writing the zeros straight into the targets' memory; every other clear stays the
// game's own.
constexpr u64 NativeClear = 0x2168630; // the game's clear (context, state, render target, colour)
constexpr std::array<u64, 3> ClearSites{0xebbc86, 0xebbca4, 0xebbcc2}; // call NativeClear
// Room for the jump to FxInitClear, in the page of the FPS++ patches' code caves (patch address
// 0x054A0000; theirs end at 0x054A00F3).
constexpr u64 Trampoline = 0x50a0f00;
constexpr u64 TrampolineSize = 16;

std::atomic<u64> fx_cleared{0};
std::atomic<u64> fx_passed{0};

// The game's render target descriptor (at +0x10 of its object) when it is one of those targets:
// 2x2 RG16 float, tiled, no compression or fast clear, at most 32 KiB, cleared to (0, 0, 0, 1)
// (only red and green are stored, so all zeros is the same picture in every tiling).
bool PlanZeroClear(const u32* target, const float* color, u64& address, u32& bytes) {
    if (color[0] != 0.0f || color[1] != 0.0f || color[2] != 0.0f || color[3] != 1.0f ||
        target[0] == 0 || target[3] != 0 || target[6] != 0x00020002 || target[7] != 0 ||
        target[8] != 0 || target[9] != 0 || target[10] != 0) {
        return false;
    }
    const u32 info = target[4], attrib = target[5];
    if (((info >> 2) & 31) != 5 || ((info >> 8) & 7) != 7 ||
        (info & (3u | (1u << 7) | (3u << 11) | (3u << 13) | (1u << 28) | (1u << 31))) != 0 ||
        (attrib & 31) != 14 || (attrib & (31u << 12)) != 0) {
        return false;
    }
    const u64 pitch = u64((target[1] & 0x7ff) + 1) * 8;
    const u64 texels = u64((target[2] & 0x3fffff) + 1) * 64;
    address = u64(target[0]) << 8;
    bytes = u32(texels * 4);
    return pitch >= 2 && texels / pitch >= 2 && texels % pitch == 0 && texels * 4 <= 32768 &&
           address + bytes <= (1ull << 40);
}

void PS4_SYSV_ABI FxInitClear(void* context, void* state, void* render_target, const float* color) {
    if (render_target && color) {
        u32 target[11];
        std::memcpy(target, static_cast<const u8*>(render_target) + 0x10, sizeof(target));
        u64 address = 0;
        u32 bytes = 0;
        if (PlanZeroClear(target, color, address, bytes)) {
            // A CPU write: the texture cache sees it like any other write by the game.
            // Breadcrumb: a crash here shows up as this step in the crash log.
            Common::Crumbs::Add("save-load fix: target cleared (address, size)", address, bytes);
            std::memset(reinterpret_cast<void*>(address), 0, bytes);
            const u64 n = fx_cleared.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3 || (n & (n - 1)) == 0) {
                LOG_INFO(Loader, "Game fix: effect history target cleared directly ({} so far)", n);
            }
            return;
        }
    }
    const u64 n = fx_passed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n & (n - 1)) == 0) {
        LOG_INFO(Loader, "Game fix: effect clear left to the game (other target, {} so far)", n);
    }
    using Clear = void(PS4_SYSV_ABI*)(void*, void*, void*, const float*);
    Common::Crumbs::Add("save-load fix: the game's own clear");
    reinterpret_cast<Clear>(At(NativeClear))(context, state, render_target, color);
}

bool InstallFxInitClear() {
    if (!Fits(Trampoline, TrampolineSize) || !Fits(NativeClear, 1)) {
        return false;
    }
    for (const u64 site : ClearSites) {
        if (!Fits(site, 5) || At(site)[0] != 0xe8) {
            return false;
        }
        s32 rel;
        std::memcpy(&rel, At(site) + 1, sizeof(rel));
        if (site + 5 + s64(rel) != NativeClear) {
            return false;
        }
    }
    // The trampoline's room must be unused (zeros).
    for (u64 i = 0; i < TrampolineSize; ++i) {
        if (At(Trampoline)[i] != 0) {
            return false;
        }
    }
    // movabs rax, FxInitClear; jmp rax
    u8* code = At(Trampoline);
    const u64 target = reinterpret_cast<u64>(&FxInitClear);
    code[0] = 0x48;
    code[1] = 0xb8;
    std::memcpy(code + 2, &target, sizeof(target));
    code[10] = 0xff;
    code[11] = 0xe0;
    for (const u64 site : ClearSites) {
        const s32 rel = s32(s64(Trampoline) - s64(site + 5));
        std::memcpy(At(site) + 1, &rel, sizeof(rel));
    }
    return true;
}

// The game's loading-screen flag: a byte the game sets to 1 when a loading screen starts and
// back to 0 when it ends. Two places set it, each with `mov byte ptr [flag], 1`: one when any
// transition starts (also quitting to the title), one when an area load starts.
constexpr u64 LoadingFlag = 0x556286b;
constexpr std::array<u64, 2> LoadingFlagWriters{0x1940d3c, 0x1929011};

std::atomic<bool> loading_flag_found{false};

bool FindLoadingFlag() {
    if (!Fits(LoadingFlag, 1)) {
        return false;
    }
    for (const u64 site : LoadingFlagWriters) {
        constexpr u64 Size = 7;
        if (!Fits(site, Size) || At(site)[0] != 0xc6 || At(site)[1] != 0x05 ||
            At(site)[6] != 0x01) {
            return false;
        }
        s32 rel;
        std::memcpy(&rel, At(site) + 2, sizeof(rel));
        if (s64(site + Size) + rel != s64(LoadingFlag)) {
            return false;
        }
    }
    return true;
}

// Screen-space reflections: Kyo's "Enable Screen Space Reflections" patch (1.09), built in so the
// reflection setting works without the patch file. Only the lines that change the game's bytes
// (the patch's other lines write what is already there). Installed at a start, and only when
// reflections aren't Off: the game then also allocates and draws its reflection passes.
struct PatchLine {
    u64 offset;
    std::string_view original;
    std::string_view patched;
    std::string_view other = {}; ///< also accepted: another patch's value here
};
constexpr std::array<PatchLine, 8> SsrLines{{
    {0x22c4fd3, "77", "74"},
    {0x22c3d2f, "7453", "9090"},
    {0x2295a5b, "CDCC4CBF", "00401CC6"},
    {0x22b8d65, "C5FA10051F0F6702", "C5FA1005C77C5C02"},
    {0xea8cfd, "0000803F", "00000000"},
    // The half-size target's format: Kyo's "Performance Patch" sets 0x37 here; with both patches
    // in the patch file the reflection patch's 0x28 wins too.
    {0xe5b1bf, "14", "28", "37"},
    {0x22c429a, "4489E1", "6A0A59"},
    {0x22c42b4, "C704240000000031C941B9140000004589E0", "8324240031C941B91400000041B80A000000"},
}};

std::atomic<SsrPatch> ssr_patch{SsrPatch::Off};

u8 HexByte(std::string_view hex, size_t i) {
    const auto digit = [](char c) -> u8 {
        return c <= '9' ? u8(c - '0') : u8((c | 0x20) - 'a' + 10);
    };
    return u8(digit(hex[i * 2]) << 4 | digit(hex[i * 2 + 1]));
}

bool Matches(const PatchLine& line, std::string_view bytes) {
    const size_t size = bytes.size() / 2;
    if (!Fits(line.offset, size)) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        if (At(line.offset)[i] != HexByte(bytes, i)) {
            return false;
        }
    }
    return true;
}

SsrPatch InstallSsr() {
    // Every line must hold the game's bytes or the patch's (the patch file may have it on).
    bool all_patched = true;
    for (const auto& line : SsrLines) {
        const bool patched = Matches(line, line.patched);
        if (!patched && !Matches(line, line.original) &&
            (line.other.empty() || !Matches(line, line.other))) {
            return SsrPatch::Unavailable;
        }
        all_patched &= patched;
    }
    if (all_patched) {
        // Players who used the patch file before reflections were built in keep them, once:
        // after that, the setting is theirs.
        if (EmulatorSettings.IsBbSsrCheckPatchFile() && EmulatorSettings.GetBbSsrMode() == 2) {
            EmulatorSettings.SetBbSsrMode(0);
            LOG_INFO(Loader, "Screen space reflections: the patch file has them on, so the "
                             "setting is now \"Game's\" (once)");
        }
        return SsrPatch::FromPatchFile;
    }
    if (EmulatorSettings.GetBbSsrMode() == 2) {
        return SsrPatch::Off;
    }
    for (const auto& line : SsrLines) {
        for (size_t i = 0; i < line.patched.size() / 2; ++i) {
            At(line.offset)[i] = HexByte(line.patched, i);
        }
    }
    return SsrPatch::Installed;
}

} // namespace

void Install() {
    if (!MemoryPatcher::g_eboot_address) {
        return;
    }
    LOG_INFO(Loader, "Game fix: save-load command buffer race {}",
             InstallFxInitClear() ? "on" : "not installed (other code there)");
    const SsrPatch ssr = InstallSsr();
    ssr_patch.store(ssr, std::memory_order_release);
    if (EmulatorSettings.IsBbSsrCheckPatchFile()) {
        EmulatorSettings.SetBbSsrCheckPatchFile(false);
        EmulatorSettings.Save();
    }
    LOG_INFO(Loader, "Screen space reflections: {}",
             ssr == SsrPatch::Installed       ? "on (built-in patch)"
             : ssr == SsrPatch::FromPatchFile ? "on (from the patch file)"
             : ssr == SsrPatch::Off           ? "off"
                                              : "not available (the game's code differs)");
    loading_flag_found.store(FindLoadingFlag(), std::memory_order_release);
    LOG_INFO(Loader, "Loading screen detection: {}",
             loading_flag_found.load(std::memory_order_relaxed)
                 ? "on"
                 : "off (the game's code differs there)");
}

SsrPatch GetSsrPatch() {
    return ssr_patch.load(std::memory_order_acquire);
}

bool SsrNeedsRestart(int ssr_mode) {
    const SsrPatch state = GetSsrPatch();
    if (state == SsrPatch::FromPatchFile || state == SsrPatch::Unavailable) {
        return false;
    }
    return (ssr_mode != 2) != (state == SsrPatch::Installed);
}

bool IsLoadingScreen() {
    if (!loading_flag_found.load(std::memory_order_acquire)) {
        return false;
    }
    return *reinterpret_cast<const volatile u8*>(At(LoadingFlag)) != 0;
}

} // namespace Core::BbGameFixes
