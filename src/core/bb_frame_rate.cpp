// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 bbport contributors (deadinside28/bloodborne_pc)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bloodborne 1.09's frame rate, changed while the game runs, built on the community's 90 FPS++
// patch (by Kyo, from shadPS4's patch repository, built in: bb_frame_rate_90fps.inc, so that it
// does not depend on the version in a user's patch file; theirs are left out).
//
// The 60 to 960 FPS++ patches are one patch: the same writes, of which only these depend on the
// frame rate r (each matches all the published rates exactly): 1/r frame times (three steps, and
// the frame pacer's target four times), counters of r/6 and r/3 frames, the pacer's wait of
// 990000/r us, 0.5/r, 3/r (four times), the frame time as five digits (1000000/r), and a table in
// the patch's own code area (0.8, 1.2 and 0.65 to the power 30/r; 30/r, +-1/r, +-1.5/r, +-3/r, r).
// Besides those, the patch sets about 70 of the game's fixed 1/30 s steps (subsystems stepped
// once a frame: scenery, effects, small simulations) to 1/60 at every rate, too fast above 60 FPS.
//
// Here: the 90 FPS++ patch is written once at the start (as published, but for the corpses' body
// parts: the game's own timers kept, see BodyPartWrites), and all of the values above are written
// for the frame rate: the pacer's for the cap (FPS cap in F1, changed at once), the steps and
// factors for the frame rate the game holds (the cap, or less while it cannot keep up). The
// game's main loop steps by the frame's real time (the patch's), its pacer waits for the cap.
//
// From bbport 0.7 (https://github.com/deadinside28/bloodborne_pc, ported earlier): the guest code
// emitter, the detours, the cloth's stiffness hook, and the per-frame ramp (0x4927fd4/0x4927fe0).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "common/bb_tuning.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/bb_frame_rate.h"
#include "core/bb_guest_code.h"
#include "core/cpu_patches.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"

namespace Core::BbFrameRate {
namespace {

using BbGuestCode::Emitter;

/// Image offsets are the game's addresses less 0x400000 (the patch file's are the addresses).
constexpr u64 ImageBase = 0x400000;
/// The farthest the patch writes (its code area's table, the frame time).
constexpr u64 ImageReach = 0x535a940;
/// The frame's time as the patched main loop measured it (clamped to 1/30 s), every frame.
constexpr u64 FrameTimeCell = 0x535a908;
/// The frame pacer's update, run once a frame on the game's thread, and its first instructions.
constexpr u64 PacerUpdate = 0x2034770;
constexpr u8 PacerEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                             0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};

/// The corpses' body parts (the physics moves them every frame, 0x1515ec0): the patch has their
/// timers and the step they are given follow the frame's time, and corpses in an area just
/// entered then move by themselves. The game's 1/30 s per frame kept (patch-file addresses).
constexpr u64 BodyPartWrites[] = {0x1915f65, 0x1915fab, 0x1916030};

/// Steps the patch sets to 1/60 that stay at it (as the patch has them at every rate): a timer
/// read twice a frame, and one other (both stepped at 1/60 by the 60 to 960 FPS++ patches).
constexpr u64 KeptSteps[] = {0x4d2577c, 0x4d27c4c};

/// The 90 FPS++ patch, as published: its values at 90 FPS, checked before any is changed.
constexpr float Rate90 = 90.0f;

enum class Kind : u8 {
    InvRate,       ///< 1/r (float)
    HalfInv,       ///< 0.5/r
    ThreeInv,      ///< 3/r
    NegThree,      ///< -3/r
    Sixth,         ///< r/6 frames (int, at least 5)
    Third,         ///< r/3 frames (int, at least 10)
    WaitUs,        ///< 990000/r us (int)
    Pow08,         ///< 0.8^(30/r)
    Pow12,         ///< 1.2^(30/r)
    Pow065,        ///< 0.65^(30/r)
    ThirtyInv,     ///< 30/r
    NegInv,        ///< -1/r
    OneHalfInv,    ///< 1.5/r
    NegOneHalfInv, ///< -1.5/r
    Rate,          ///< r
};

struct Value {
    u64 address; ///< patch-file address
    Kind kind;
    bool pacer; ///< the cap's (the pacer's), else the held frame rate's
};

/// The values the 60 to 960 FPS++ patches set for their rate (besides the 1/60 steps).
constexpr u64 Table = 0x5493000;
constexpr Value Values[] = {
    {0xf383ca, Kind::InvRate, false},
    {0x1972a80, Kind::InvRate, false},
    {0x1c0ca9a, Kind::InvRate, false},
    {0x1929676, Kind::Sixth, false},
    {0x192c405, Kind::Third, false},
    {0x2434845, Kind::InvRate, true}, // the pacer's frame time (its four modes)
    {0x243485f, Kind::InvRate, true},
    {0x2434883, Kind::InvRate, true},
    {0x24348b8, Kind::InvRate, true},
    {0x25629f1, Kind::WaitUs, true},
    {0x25df519, Kind::HalfInv, false},
    {0x4cf6420, Kind::ThreeInv, false},
    {0x4cf6424, Kind::ThreeInv, false},
    {0x4cf6428, Kind::ThreeInv, false},
    {0x4cf642c, Kind::ThreeInv, false},
    {Table + 0, Kind::Pow08, false},
    {Table + 16, Kind::Pow12, false},
    {Table + 32, Kind::Pow065, false},
    {Table + 48, Kind::ThirtyInv, false},
    {Table + 64, Kind::NegInv, false},
    {Table + 80, Kind::OneHalfInv, false},
    {Table + 96, Kind::NegOneHalfInv, false},
    {Table + 112, Kind::Rate, false},
    {Table + 116, Kind::Rate, false},
    {Table + 120, Kind::Rate, false},
    {Table + 124, Kind::Rate, false},
    {Table + 128, Kind::Rate, false},
    {Table + 132, Kind::Rate, false},
    {Table + 136, Kind::Rate, false},
    {Table + 140, Kind::Rate, false},
    {Table + 144, Kind::Rate, false},
    {Table + 160, Kind::Rate, false},
    {Table + 176, Kind::Rate, false},
    {Table + 192, Kind::Rate, false},
    {Table + 208, Kind::InvRate, false},
    {Table + 224, Kind::InvRate, false},
    {Table + 240, Kind::NegThree, false},
    {Table + 256, Kind::ThreeInv, false},
};
/// The frame time as five digits (1000000/r us), the pacer's.
constexpr u64 FrameTimeText = 0x4b33c51;
/// The pacer's own target (one of the patch's 1/60 writes).
constexpr u64 PacerTarget = 0x243460c;
/// A value the game ramps by 0.1 a 30 FPS frame (down, up), not in the patch (bbport's).
constexpr u64 RampDown = 0x4d27fd4, RampUp = 0x4d27fe0;

/// The follow camera (0x183ac60 on): the patch's 12 jumps from its code into the patch's own
/// routines are left out, the game's camera code kept, and the two weights its follow eases by
/// each frame (`x += (target - x) * w`, w for 30 FPS: 0.1, read at 0x183af8c, and 1/30, read at
/// 0x183e2b2) converted for the frame rate, 1 - (1 - w)^(30/r): the camera as the game has it.
constexpr u64 CameraCode = 0x183a000, CameraCodeEnd = 0x1841000;
constexpr u64 EaseDistance = 0x4d25e68, EaseChase = 0x4d25e94;
constexpr float EaseDistanceGame = 0.1f, EaseChaseGame = 1.0f / 30.0f;
bool camera_eases = false; ///< the game's camera kept: its weights follow the frame rate

/// The patch's main step stores the frame's measured time for the rest of the game to read
/// (`movss [0x575a908], xmm0`): it moves 15% of the way to it each frame instead, so that uneven
/// frames do not jolt what reads it (the main step itself keeps the measured time).
constexpr u64 FrameTimeStore = 0x2418e57;
constexpr u8 FrameTimeStoreCode[] = {0xf3, 0x0f, 0x11, 0x05, 0xa9, 0x1a, 0x34, 0x03};
constexpr float FrameTimeEase = 0.15f;

constexpr int MinRate = 30;
/// The highest frame rate: the game breaks above it (bbport: on loading screens at 880-1000 FPS
/// its main loop outran its worker threads; at 480 all loads went through).
constexpr int MaxRate = 480;

u64 image_base = 0;
std::atomic<bool> active{false};
std::atomic<int> target{60};         ///< the cap (MaxRate: none)
std::atomic<float> held_rate{60.0f}; ///< the frame rate the game holds (smoothed)
std::atomic<float> step_rate{60.0f}; ///< the frame rate the steps are set for
float held = 60.0f;
int written_target = 0;
float written_rate = 0.0f;
int drift_frames = 0;
std::vector<u64> steps; ///< the 1/60 steps set to 1/r (patch-file addresses)
struct PatchWrite {
    u64 address; ///< patch-file address
    u32 size;
    u32 at; ///< in NinetyBytes
};
#include "core/bb_frame_rate_90fps.inc"

/// The built-in 90 FPS++ patch's writes.
std::vector<std::pair<u64, std::vector<u8>>> patch = [] {
    std::vector<std::pair<u64, std::vector<u8>>> writes;
    for (const PatchWrite& w : NinetyWrites) {
        writes.emplace_back(w.address,
                            std::vector<u8>(NinetyBytes + w.at, NinetyBytes + w.at + w.size));
    }
    return writes;
}();

u8* At(u64 address) {
    return reinterpret_cast<u8*>(image_base + (address - ImageBase));
}

float F32(const u8* at) {
    float f;
    std::memcpy(&f, at, 4);
    return f;
}

void Store(u64 address, const void* value, std::size_t size) {
    std::memcpy(At(address), value, size);
}

void StoreF32(u64 address, float f) {
    Store(address, &f, 4);
}

float Evaluate(Kind kind, float r) {
    switch (kind) {
    case Kind::InvRate:
        return 1.0f / r;
    case Kind::HalfInv:
        return 0.5f / r;
    case Kind::ThreeInv:
        return 3.0f / r;
    case Kind::NegThree:
        return -3.0f / r;
    case Kind::Pow08:
        return std::pow(0.8f, 30.0f / r);
    case Kind::Pow12:
        return std::pow(1.2f, 30.0f / r);
    case Kind::Pow065:
        return std::pow(0.65f, 30.0f / r);
    case Kind::ThirtyInv:
        return 30.0f / r;
    case Kind::NegInv:
        return -1.0f / r;
    case Kind::OneHalfInv:
        return 1.5f / r;
    case Kind::NegOneHalfInv:
        return -1.5f / r;
    case Kind::Rate:
        return r;
    default:
        return 0.0f;
    }
}

s32 EvaluateInt(Kind kind, float r) {
    switch (kind) {
    case Kind::Sixth:
        return std::max(5, int(r / 6.0f));
    case Kind::Third:
        return std::max(10, int(r / 3.0f));
    case Kind::WaitUs:
        return s32(990000.0f / r);
    default:
        return 0;
    }
}

bool IsInt(Kind kind) {
    return kind == Kind::Sixth || kind == Kind::Third || kind == Kind::WaitUs;
}

void WriteValue(const Value& v, float r) {
    if (IsInt(v.kind)) {
        const s32 i = EvaluateInt(v.kind, r);
        Store(v.address, &i, 4);
    } else {
        StoreF32(v.address, Evaluate(v.kind, r));
    }
}

/// The patch as published holds its 90 FPS values where they are expected (another version of
/// it, or another patch, would be changed blindly otherwise).
bool PatchIsExpected() {
    for (const Value& v : Values) {
        if (IsInt(v.kind)) {
            s32 i;
            std::memcpy(&i, At(v.address), 4);
            if (i != EvaluateInt(v.kind, Rate90)) {
                return false;
            }
        } else if (std::fabs(F32(At(v.address)) - Evaluate(v.kind, Rate90)) >
                   1e-4f * std::max(1.0f, std::fabs(Evaluate(v.kind, Rate90)))) {
            return false;
        }
    }
    return std::memcmp(At(FrameTimeText), "11111", 5) == 0 &&
           std::fabs(F32(At(RampDown)) + 0.1f) < 1e-6f && std::fabs(F32(At(RampUp)) - 0.1f) < 1e-6f;
}

/// The pacer's values for the cap.
void WritePacer(int cap) {
    const float r = float(cap);
    for (const Value& v : Values) {
        if (v.pacer) {
            WriteValue(v, r);
        }
    }
    StoreF32(PacerTarget, 1.0f / r);
    char text[8];
    std::snprintf(text, sizeof(text), "%05d", std::min(99999, int(1000000.0f / r)));
    Store(FrameTimeText, text, 5);
    written_target = cap;
}

/// The steps and per-frame factors for the frame rate the game holds.
void WriteSteps(float r) {
    for (const Value& v : Values) {
        if (!v.pacer) {
            WriteValue(v, r);
        }
    }
    for (const u64 address : steps) {
        StoreF32(address, 1.0f / r);
    }
    StoreF32(RampDown, -3.0f / r);
    StoreF32(RampUp, 3.0f / r);
    if (camera_eases) {
        StoreF32(EaseDistance, 1.0f - std::pow(1.0f - EaseDistanceGame, 30.0f / r));
        StoreF32(EaseChase, 1.0f - std::pow(1.0f - EaseChaseGame, 30.0f / r));
    }
    written_rate = r;
    step_rate.store(r, std::memory_order_relaxed);
}

/// The frame rate the steps follow: the cap while the game holds it, else what it holds (in
/// steps of 5 FPS, so that they are not rewritten every frame).
float Wanted(float rate, int cap) {
    if (rate >= float(cap) * 0.95f) {
        return float(cap);
    }
    return std::clamp(std::round(rate / 5.0f) * 5.0f, float(MinRate), float(cap));
}

/// Each frame, at the pacer's update (the game's thread, before the pacer's code runs).
void PS4_SYSV_ABI FrameTick() {
    const int cap = target.load(std::memory_order_relaxed);
    if (cap != written_target) {
        WritePacer(cap);
    }
    const float frame = F32(At(FrameTimeCell + ImageBase));
    if (frame > 0.0f && std::isfinite(frame)) {
        const float rate = std::clamp(1.0f / frame, float(MinRate), float(cap));
        held += (rate - held) * 0.1f;
        held_rate.store(held, std::memory_order_relaxed);
    }
    const float wanted = Wanted(held, cap);
    if (wanted == written_rate) {
        drift_frames = 0;
        return;
    }
    // A cap change at once; a change of the held frame rate once it lasts (rising sooner).
    const bool cap_change = written_rate > float(cap) || wanted == float(cap);
    if (cap_change || ++drift_frames >= (wanted > written_rate ? 30 : 60)) {
        WriteSteps(wanted);
        drift_frames = 0;
    }
}

// ---- Detours: the instructions at a site replaced by a jump to a stub ----
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

/// `e` into the stub page; a jump to it at image offset `site` over `length` bytes (the rest int3)
/// if the site holds `expected`.
bool Detour(u64 site, const u8* expected, std::size_t length, const Emitter& e) {
    u8* code = reinterpret_cast<u8*>(image_base + site);
    if (length < 5 || std::memcmp(code, expected, length) != 0 || e.overflow ||
        stub_used + e.size > StubPageSize) {
        return false;
    }
    u8* stub = stub_page + stub_used;
    const s64 rel = s64(reinterpret_cast<u64>(stub)) - s64(image_base + site + 5);
    if (rel != s32(rel)) {
        return false;
    }
    std::memcpy(stub, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    const s32 rel32 = s32(rel);
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel32, 4);
    for (std::size_t i = 5; i < length; ++i) {
        code[i] = 0xcc;
    }
    return true;
}

/// FrameTick at the pacer's update, then its first instructions.
bool HookPacer() {
    Emitter e;
    e.SaveVolatile();
    e.CallAbs(reinterpret_cast<const void*>(&FrameTick));
    e.RestoreVolatile();
    for (const u8 b : PacerEntry) {
        e.Bytes({b});
    }
    e.JmpAbs(image_base + PacerUpdate + sizeof(PacerEntry));
    return Detour(PacerUpdate, PacerEntry, sizeof(PacerEntry), e);
}

/// The eased frame time at the patch's store of it (FrameTimeStore).
bool HookFrameTimeEase() {
    const u64 site = FrameTimeStore - ImageBase;
    const u64 cell = image_base + FrameTimeCell;
    Emitter e;
    e.Bytes({0x48, 0x8d, 0x64, 0x24, 0x80}); // lea rsp, [rsp - 128] (past the red zone)
    e.Bytes({0x50});                         // push rax
    e.Bytes({0x48, 0x8d, 0x64, 0x24, 0xe0}); // lea rsp, [rsp - 32]
    e.Bytes({0x0f, 0x11, 0x0c, 0x24});       // movups [rsp], xmm1
    e.Bytes({0x0f, 0x11, 0x54, 0x24, 0x10}); // movups [rsp + 16], xmm2
    e.Bytes({0x48, 0xb8});                   // movabs rax, cell
    e.U64(cell);
    e.Bytes({0xf3, 0x0f, 0x10, 0x08}); // movss xmm1, [rax]
    e.Bytes({0x0f, 0x28, 0xd0});       // movaps xmm2, xmm0
    e.Bytes({0xf3, 0x0f, 0x5c, 0xd1}); // subss xmm2, xmm1
    e.Bytes({0xf3, 0x0f, 0x59, 0x15}); // mulss xmm2, [rip + ease]
    const std::size_t ease_disp = e.size;
    e.Bytes({0, 0, 0, 0});
    const std::size_t after_mul = e.size;
    e.Bytes({0xf3, 0x0f, 0x58, 0xca});                         // addss xmm1, xmm2
    e.Bytes({0xf3, 0x0f, 0x11, 0x08});                         // movss [rax], xmm1
    e.Bytes({0x0f, 0x10, 0x0c, 0x24});                         // movups xmm1, [rsp]
    e.Bytes({0x0f, 0x10, 0x54, 0x24, 0x10});                   // movups xmm2, [rsp + 16]
    e.Bytes({0x48, 0x8d, 0x64, 0x24, 0x20});                   // lea rsp, [rsp + 32]
    e.Bytes({0x58});                                           // pop rax
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
    e.JmpAbs(image_base + site + sizeof(FrameTimeStoreCode));
    const s32 disp = s32(e.size - after_mul);
    u8 ease[4];
    std::memcpy(ease, &FrameTimeEase, 4);
    e.Bytes({ease[0], ease[1], ease[2], ease[3]});
    std::memcpy(e.code.data() + ease_disp, &disp, 4);
    return Detour(site, FrameTimeStoreCode, sizeof(FrameTimeStoreCode), e);
}

/// The game's exponent of Havok's stiffness adaptation (0x48d7bbc: -1.725), read at start.
float cloth_exponent = -1.725f;

/// Havok's stiffness factor of a constraint set for one substep (0x1167a80, replaced; bbport):
/// `mode` 2 adapts the constraints' stiffness to how often they are solved, (substeps x a x
/// b)^-1.725, for a set of type 8 the iteration's share instead and for types 5 and 10 only the
/// last substep. The substeps are per frame: at a higher frame rate the cloth is solved more often
/// by the same factor and is stiffer by as much (it flutters). The factor here is the game's x
/// the frame's length in 30 FPS frames: the cloth is pulled as far per second as at 30 FPS.
float PS4_SYSV_ABI ClothStiffnessFactor(const u8* set, int mode, int iteration, int substeps,
                                        float a, float b) {
    const u32 type = *reinterpret_cast<const u32*>(set + 0x18);
    const bool adapts = mode == 2 && !(substeps == 1 && a == 1.0f && b == 1.0f);
    if (adapts && type == 8) {
        return float(iteration + 1) / float(substeps);
    }
    if (adapts && (type == 5 || type == 10)) {
        return substeps - 1 == iteration ? 1.0f : 0.0f;
    }
    const float game = adapts ? std::pow(float(substeps) * a * b, cloth_exponent) : 1.0f;
    if (type == 5 || type == 8 || type == 10) {
        return game;
    }
    return game * (30.0f / step_rate.load(std::memory_order_relaxed));
}

/// Havok's stiffness factor (0x1167a80: `push rbp; mov rbp, rsp; push r15`) by
/// ClothStiffnessFactor (its arguments as they are, a tail call).
bool HookClothStiffness() {
    constexpr u64 site = 0x1167a80;
    constexpr u8 code[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    std::memcpy(&cloth_exponent, reinterpret_cast<const void*>(image_base + 0x48d7bbc),
                sizeof(float));
    if (!(cloth_exponent < 0.0f && cloth_exponent > -4.0f)) {
        return false;
    }
    Emitter e;
    e.JmpAbs(reinterpret_cast<u64>(&ClothStiffnessFactor));
    return Detour(site, code, sizeof(code), e);
}

/// The pages of `address` to `address + size` writable (the values are rewritten while the game
/// runs: code and read-only data among them).
bool Unlock(u64 address, std::size_t size) {
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(At(address));
    const std::uintptr_t first = at & ~std::uintptr_t(0xfff);
    const std::uintptr_t end = (at + size + 0xfff) & ~std::uintptr_t(0xfff);
#ifdef _WIN32
    DWORD before = 0;
    return VirtualProtect(reinterpret_cast<void*>(first), end - first, PAGE_EXECUTE_READWRITE,
                          &before) != 0;
#else
    return mprotect(reinterpret_cast<void*>(first), end - first,
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

bool deciding = false;
/// Patches of the patch file that write where the 90 FPS++ patch does: frame rate patches when in
/// at least FrameRatePatchWrites places (the 30 to 960 FPS++ patches of any version: 80 or more;
/// others that touch a few, Performance Patch 4, Enemy Control 2, are kept: the frame rate's values
/// written after them).
constexpr int FrameRatePatchWrites = 20;
std::map<std::string, int, std::less<>> patch_overlaps;

int Cap(int fps) {
    return fps > 0 ? std::clamp(fps, MinRate, MaxRate) : MaxRate;
}

} // namespace

void Prepare() {
    patch_overlaps.clear();
    auto* param_sfo = Common::Singleton<PSF>::Instance();
    const auto version = param_sfo->GetString("APP_VER").value_or("");
    deciding = EmulatorSettings.IsBbFrameHooks() &&
               Common::BbTuning::IsBloodborne(MemoryPatcher::g_game_serial) && version == "01.09" &&
               MemoryPatcher::g_eboot_name == "eboot.bin";
}

void CheckPatch(std::string_view name, u64 address, u64 size) {
    if (!deciding) {
        return;
    }
    for (const auto& [at, bytes] : patch) {
        if (address < at + bytes.size() && address + size > at) {
            auto it = patch_overlaps.find(name);
            if (it == patch_overlaps.end()) {
                it = patch_overlaps.emplace(std::string{name}, 0).first;
            }
            if (++it->second == FrameRatePatchWrites) {
                LOG_INFO(Loader, "Frame rate: patch {} skipped (a frame rate patch)", name);
            }
            return;
        }
    }
}

bool ShouldSkipPatch(std::string_view name) {
    if (!deciding) {
        return false;
    }
    const auto it = patch_overlaps.find(name);
    return it != patch_overlaps.end() && it->second >= FrameRatePatchWrites;
}

void Install() {
    if (!deciding || !MemoryPatcher::g_eboot_address) {
        return;
    }
    image_base = MemoryPatcher::g_eboot_address;
    if (MemoryPatcher::g_eboot_image_size < ImageReach) {
        LOG_WARNING(Loader, "Frame rate: not unlocked (not Bloodborne 1.09's image)");
        return;
    }
    stub_page = static_cast<u8*>(
        AllocatePatchTrampolineSpace(reinterpret_cast<const void*>(image_base), StubPageSize));
    if (!stub_page || std::memcmp(reinterpret_cast<const void*>(image_base + PacerUpdate),
                                  PacerEntry, sizeof(PacerEntry)) != 0) {
        LOG_WARNING(Loader, "Frame rate: not unlocked (the frame pacer is not the expected code)");
        return;
    }
    // The patch as published, but the body parts' writes.
    steps.clear();
    for (const auto& [address, bytes] : patch) {
        if (address < ImageBase || address - ImageBase + bytes.size() > ImageReach) {
            LOG_WARNING(Loader, "Frame rate: not unlocked (90 FPS++ writes outside the game)");
            return;
        }
    }
    // The game's camera weights, before any is changed (else the patch's camera is kept).
    const bool game_camera = std::fabs(F32(At(EaseDistance)) - EaseDistanceGame) < 1e-6f &&
                             std::fabs(F32(At(EaseChase)) - EaseChaseGame) < 1e-6f;
    int kept = 0, camera = 0;
    for (const auto& [address, bytes] : patch) {
        if (std::find(std::begin(BodyPartWrites), std::end(BodyPartWrites), address) !=
            std::end(BodyPartWrites)) {
            ++kept;
            continue;
        }
        if (game_camera && address >= CameraCode && address < CameraCodeEnd) {
            ++camera;
            continue;
        }
        Store(address, bytes.data(), bytes.size());
        constexpr u8 sixtieth[] = {0x89, 0x88, 0x88, 0x3c};
        if (bytes.size() == 4 && std::memcmp(bytes.data(), sixtieth, 4) == 0 &&
            address != PacerTarget &&
            std::find(std::begin(KeptSteps), std::end(KeptSteps), address) == std::end(KeptSteps)) {
            steps.push_back(address);
        }
    }
    if (!PatchIsExpected()) {
        LOG_ERROR(Loader, "Frame rate: the 90 FPS++ patch is not the expected version; it is "
                          "applied as it is, at 90 FPS");
        return;
    }
    // All the values rewritten while the game runs: their pages writable.
    bool unlocked = Unlock(PacerTarget, 4) && Unlock(FrameTimeText, 5) && Unlock(RampDown, 4) &&
                    Unlock(RampUp, 4) && Unlock(EaseDistance, 4) && Unlock(EaseChase, 4);
    for (const Value& v : Values) {
        unlocked = unlocked && Unlock(v.address, 4);
    }
    for (const u64 address : steps) {
        unlocked = unlocked && Unlock(address, 4);
    }
    if (!unlocked) {
        LOG_ERROR(Loader, "Frame rate: its values cannot be changed while the game runs; 90 FPS");
        return;
    }
    const int cap = Cap(EmulatorSettings.GetBbFpsLimit());
    target.store(cap, std::memory_order_relaxed);
    held = float(cap);
    camera_eases = game_camera;
    StoreF32(FrameTimeCell + ImageBase, 1.0f / float(cap));
    WritePacer(cap);
    WriteSteps(float(cap));
    const bool cloth = HookClothStiffness();
    const bool eased = HookFrameTimeEase();
    if (!HookPacer()) {
        LOG_ERROR(Loader, "Frame rate: the frame pacer's hook did not fit; {} FPS fixed", cap);
        return;
    }
    active.store(true, std::memory_order_release);
    LOG_INFO(Loader,
             "Frame rate: 90 FPS++ (by Kyo) with its values for {} FPS: {} writes ({} of the body "
             "parts' kept), {} steps follow the frame rate, the camera {}, the frame time {}, the "
             "cloth's stiffness {}",
             cap == MaxRate ? std::string("uncapped") : std::to_string(cap),
             patch.size() - kept - camera, kept, steps.size(),
             game_camera && camera == 12 ? "the game's, its eases converted"
             : game_camera               ? "the game's (not all 12 of the patch's camera writes)"
                                         : "the patch's (the game's weights differ)",
             eased ? "eased" : "as measured (the store is not the expected code)",
             cloth ? "by solves per second" : "per frame (not found)");
}

bool Active() {
    return active.load(std::memory_order_acquire);
}

float CapPeriod() {
    if (!Active()) {
        return 0.0f;
    }
    const int cap = target.load(std::memory_order_relaxed);
    return cap >= MaxRate ? 0.0f : 1.0f / float(cap);
}

void SetLimit(int fps) {
    target.store(Cap(fps), std::memory_order_relaxed);
    if (Active()) {
        LOG_INFO(Loader, "Frame rate: cap {}", fps > 0 ? std::to_string(Cap(fps)) : "none");
    }
}

float HeldRate() {
    return Active() ? held_rate.load(std::memory_order_relaxed) : 0.0f;
}

} // namespace Core::BbFrameRate
