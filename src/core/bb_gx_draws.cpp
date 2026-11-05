// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// GX's draw methods and the pending draw state as the 1.09 executable lays them out, from
// bbhost's maps (github.com/droogie/bbhost: src/decomp/gx/draw.cpp, src/hle/gx_trace.cpp,
// src/engine/gx_state.h), checked here against the executable's code. Addresses are the game's
// (the image at 0x400000).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <fmt/format.h>

#include "common/bb_tuning.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/singleton.h"
#include "core/bb_guest_code.h"
#include "core/bb_gx_draws.h"
#include "core/cpu_patches.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "video_core/amdgpu/regs.h"

namespace Core::BbGxDraws {
namespace {

using BbGuestCode::Emitter;

constexpr u64 ImageBase = 0x400000;
/// GX's topology (D3D11's) to the GPU's primitive type: a table of 0x2a dwords.
constexpr u64 TopologyTable = 0x4c6fb20;
constexpr u32 TopologyCount = 0x2a;
/// The blend state an unset slot stands for.
constexpr u64 DefaultBlend = 0x586c8f4;

/// The pending draw state (the draw methods' second argument).
namespace State {
constexpr u64 InputLayout = 0x0;
constexpr u64 Topology = 0x8;
constexpr u64 IndexBuffer = 0x10;
constexpr u64 IndexFormat = 0x18; ///< DXGI: 0x2a R32_UINT, else 16-bit
constexpr u64 IndexOffset = 0x1c;
constexpr u64 VertexBuffers = 0x20;  ///< [32] buffer objects
constexpr u64 VertexStrides = 0x120; ///< [32] dwords
constexpr u64 Raster = 0x240;
constexpr u64 Blend = 0x4d0;
constexpr u64 DepthStencil = 0x4d8;
constexpr u64 Targets = 0x4e0; ///< [8] render target views
constexpr u64 DepthTarget = 0x520;
/// Six stages in D3D11's order (VS HS DS GS PS CS), 0x4f8 bytes each: samplers[16], views[128],
/// constant buffers[14], then the shader object.
constexpr u64 Stages = 0x598;
constexpr u64 StageSize = 0x4f8;
constexpr u64 StageSamplers = 0x0; ///< [16]
constexpr u64 StageViews = 0x80;   ///< [128] shader-resource views
constexpr u64 StageShader = 0x4f0;
} // namespace State
/// A shader object's Gnm shader structure (+0x100); its registers from +8: PGM_LO, PGM_HI.
constexpr u64 ShaderGnm = 0x100;
/// An index buffer object: renamed (its data in GX's upload heap) when the low byte at +0x40 is
/// set; else its memory at +0x20.
constexpr u64 BufferRenamed = 0x40;
constexpr u64 BufferMemory = 0x20;
/// The context's command buffer (+0x10): begin, end, write pointer.
constexpr u64 ContextWritePointer = 0x20;

/// The draw methods where the draw packet's writer returns (bbhost's kGameReturns): the six
/// bytes there (two register moves, relocated into the stub), and which registers hold the
/// context and the pending state there.
enum Reg : u8 { Rbx, R12, R13, R14, R15 };
struct Site {
    const char* name;
    u64 address;
    std::array<u8, 6> code;
    Reg context;
    Reg state;
};
constexpr std::array<Site, 6> Sites{{
    {"Draw", 0x2569753, {0x4c, 0x89, 0xf7, 0x48, 0x89, 0xde}, Rbx, R12},
    {"DrawIndexed", 0x256981c, {0x4c, 0x89, 0xff, 0x48, 0x89, 0xde}, Rbx, R13},
    {"DrawInstanced", 0x25698d4, {0x4c, 0x89, 0xf7, 0x48, 0x89, 0xde}, Rbx, R13},
    {"DrawIndexedInstanced", 0x25699a1, {0x4c, 0x89, 0xff, 0x4c, 0x89, 0xf6}, R14, Rbx},
    {"DrawInstancedIndirect", 0x2569a8e, {0x4c, 0x89, 0xe7, 0x48, 0x89, 0xde}, Rbx, R15},
    {"DrawIndexedInstancedIndirect", 0x2569b71, {0x4c, 0x89, 0xe7, 0x48, 0x89, 0xde}, Rbx, R15},
}};
constexpr bool Indexed(u32 kind) {
    return kind == 1 || kind == 3 || kind == 5;
}

/// What a draw call bound, as the hook read it.
struct Capture {
    std::array<u32, 2> tail{}; ///< the draw packet's last two dwords then (a newer draw there)
    u8 kind{};
    bool tess{};
    bool gs{}; ///< a geometry shader: GX's vertex shader runs as the export stage
    bool has_ps{};
    bool ib_known{};
    bool prim_known{};
    u64 vs{}; ///< the hardware vertex stage's program (the domain shader's with tessellation)
    u64 ps{};
    u32 prim{};
    u64 ib{};
    u32 ib_type{};
    u32 target_mask{};
    /// The draw's pipeline told by the engine's objects, three ways (MemoKeys).
    std::array<u64, 3> memo{};
};

/// Pipeline memo check: whether the engine's objects tell a draw's pipeline, so that a later step
/// can skip working it out from the registers (the command thread's "pipeline select", about a
/// third of its time a draw). Three keys, each adding to the one before: the objects (shaders,
/// input layout, topology, blend, depth-stencil and rasterizer states, targets); with the
/// vertex and pixel shaders' views and samplers (the shaders are specialised by their formats);
/// with the vertex buffers' strides and kinds. A key that never tells two pipelines apart is
/// enough.
constexpr std::array<const char*, 3> MemoKeys{"objects", "+views", "+vertex buffers"};

u64 image_base = 0;
std::atomic<bool> hooked{false};
bool check_fields = false;  ///< "Check GX draw capture": compare the fields with the registers
bool use_pipelines = false; ///< "Pipelines from the engine's objects"
bool reuse_stopped = false; ///< a spot check differed: worked out from the registers again

template <typename T>
T Read(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}
u64 Game(u64 address) {
    return image_base + (address - ImageBase);
}

// Captures by where their draw packet ends, in shards (the game draws from several threads).
constexpr size_t ShardCount = 64;
constexpr size_t ShardLimit = 1 << 15;
struct alignas(64) Shard {
    std::mutex mutex;
    std::unordered_map<u64, Capture> captures;
};
std::array<Shard, ShardCount> shards;
Shard& ShardOf(u64 key) {
    return shards[(key >> 3) * 0x9e3779b97f4a7c15ull >> 58];
}

// Counters.
std::array<std::atomic<u64>, Sites.size()> calls{};
std::atomic<u64> captures_dropped{0};
enum Field { Vs, Ps, Prim, Index, IndexType, TargetMask, FieldCount };
constexpr std::array<const char*, FieldCount> FieldNames{
    "vertex shader", "pixel shader", "topology", "index buffer", "index size", "colour mask"};
struct Counts {
    u64 draws{};    ///< draw packets the command thread ran
    u64 captured{}; ///< ... with a capture
    u64 stale{};    ///< ... whose capture was of a newer draw at the same place
    u64 tess{};     ///< ... tessellated
    u64 gs{};       ///< ... with a geometry shader
    /// Pipeline memo by key: draws whose key was known and gave the same pipeline, a different
    /// one; and new keys.
    std::array<u64, 3> memo_same{};
    std::array<u64, 3> memo_differ{};
    std::array<u64, 3> memo_new{};
    u64 reused{};      ///< draws that took the remembered pipeline
    u64 spot_checks{}; ///< ... of which worked out again to compare
    u64 spot_differed{};
    std::array<u64, FieldCount> same{};
    std::array<u64, FieldCount> differ{};
};
Counts counts; ///< the command thread's only
/// The command thread's: the capture of the draw it is running (for NotePipeline), and the
/// memos.
bool current_valid = false;
std::array<u64, 3> current_memo{};
std::array<std::unordered_map<u64, const void*>, 3> memos;
/// The key the pipelines are reused by ("+views": no draw of a whole play-through told a
/// different pipeline), and the generation of the pipeline cache the memos are of.
constexpr size_t UseKey = 1;
u64 memo_generation = 0;
u32 spot_counter = 0;
const void* spot_pending = nullptr; ///< the remembered pipeline of a spot-checked draw
constexpr u32 SpotEvery = 256;
constexpr size_t MemoLimit = 1 << 18;
std::chrono::steady_clock::time_point counts_start{};
u32 differences_logged = 0;
/// Spot checks that found a different pipeline: the key is set aside (always worked out from
/// the registers) and reuse goes on; past MaxDiffers, reuse stops for the session.
std::unordered_set<u64> unreliable_keys;
u32 spot_differences = 0;
constexpr u32 MaxDiffers = 16;
/// The render target signature of the draw being looked up (RememberedPipeline/NotePipeline).
u64 current_targets = 0;
u64 KeyFor(size_t k) {
    return current_memo[k] ^ (current_targets * 0x9e3779b97f4a7c15ull);
}

bool Unlock(u64 address, std::size_t size) {
    const std::uintptr_t first = address & ~std::uintptr_t(0xfff);
    const std::uintptr_t end = (address + size + 0xfff) & ~std::uintptr_t(0xfff);
#ifdef _WIN32
    DWORD before = 0;
    return VirtualProtect(reinterpret_cast<void*>(first), end - first, PAGE_EXECUTE_READWRITE,
                          &before) != 0;
#else
    return mprotect(reinterpret_cast<void*>(first), end - first,
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

u64 ProgramAddress(u64 shader) {
    if (!shader) {
        return 0;
    }
    const u64 gnm = Read<u64>(shader + ShaderGnm);
    if (!gnm) {
        return 0;
    }
    const u32 lo = Read<u32>(gnm + 8);
    const u32 hi = Read<u32>(gnm + 12);
    return ((u64(hi & 0xff) << 32) | lo) << 8;
}

/// From the game's thread, where the draw packet's writer has returned.
void PS4_SYSV_ABI OnDraw(u64 context, u64 state, u32 kind) {
    calls[kind].fetch_add(1, std::memory_order_relaxed);
    // The writer ends a draw with a two-dword packet (an event or a NOP): the draw before it.
    const u64 write = Read<u64>(context + ContextWritePointer);
    u64 end = write;
    const u32 last = Read<u32>(write - 8);
    const u32 opcode = (last >> 8) & 0xff;
    if ((last >> 30) == 3 && ((last >> 16) & 0x3fff) == 0 && (opcode == 0x10 || opcode == 0x46)) {
        end = write - 8;
    }
    Capture c;
    c.kind = u8(kind);
    c.tail = {Read<u32>(end - 8), Read<u32>(end - 4)};
    const auto stage = [&](u64 k) {
        return Read<u64>(state + State::Stages + State::StageSize * k + State::StageShader);
    };
    const u64 vertex = stage(0), hull = stage(1), domain = stage(2), geometry = stage(3),
              pixel = stage(4);
    c.tess = hull || domain;
    c.gs = geometry != 0;
    c.vs = ProgramAddress(c.tess ? domain : vertex);
    c.has_ps = pixel != 0;
    c.ps = ProgramAddress(pixel);
    const u32 topology = Read<u32>(state + State::Topology);
    if (topology < TopologyCount) {
        c.prim = Read<u32>(Game(TopologyTable) + 4 * u64(topology));
        c.prim_known = true;
    }
    if (Indexed(kind)) {
        if (const u64 buffer = Read<u64>(state + State::IndexBuffer);
            buffer && (Read<u32>(buffer + BufferRenamed) & 0xff) == 0) {
            c.ib = Read<u64>(buffer + BufferMemory) + Read<u32>(state + State::IndexOffset);
            c.ib_type = Read<u32>(state + State::IndexFormat) == 0x2a ? 1 : 0;
            c.ib_known = true;
        }
    }
    const u64 blend = Read<u64>(state + State::Blend);
    c.target_mask = Read<u32>(blend ? blend : Game(DefaultBlend));
    {
        // The memo keys: FNV-style mixing of the objects' addresses.
        u64 h = 0xcbf29ce484222325ull;
        const auto mix = [&](u64 v) { h = (h ^ v) * 0x100000001b3ull; };
        for (const u64 shader : {vertex, hull, domain, geometry, pixel}) {
            mix(shader);
        }
        mix(Read<u64>(state + State::InputLayout));
        mix(topology);
        mix(blend);
        mix(Read<u64>(state + State::DepthStencil));
        mix(Read<u64>(state + State::Raster));
        for (u64 i = 0; i < 8; ++i) {
            mix(Read<u64>(state + State::Targets + 8 * i));
        }
        mix(Read<u64>(state + State::DepthTarget));
        c.memo[0] = h;
        for (const u64 k : {u64(0), u64(4)}) { // the vertex and pixel shaders' bindings
            const u64 group = state + State::Stages + State::StageSize * k;
            for (u64 i = 0; i < 16; ++i) {
                if (const u64 sampler = Read<u64>(group + State::StageSamplers + 8 * i)) {
                    mix(i ^ (sampler << 8));
                }
            }
            for (u64 i = 0; i < 128; ++i) {
                if (const u64 view = Read<u64>(group + State::StageViews + 8 * i)) {
                    mix(i ^ (view << 8));
                }
            }
        }
        c.memo[1] = h;
        for (u64 i = 0; i < 32; ++i) {
            const u64 buffer = Read<u64>(state + State::VertexBuffers + 8 * i);
            if (!buffer) {
                continue;
            }
            const bool renamed = (Read<u32>(buffer + BufferRenamed) & 0xff) != 0;
            mix(i | u64(Read<u32>(state + State::VertexStrides + 4 * i)) << 8 | u64(renamed) << 40);
        }
        c.memo[2] = h;
    }

    Shard& shard = ShardOf(end);
    std::scoped_lock lock{shard.mutex};
    if (shard.captures.size() >= ShardLimit) {
        captures_dropped.fetch_add(shard.captures.size(), std::memory_order_relaxed);
        shard.captures.clear();
    }
    shard.captures.insert_or_assign(end, c);
}

bool Hook(u8* stubs, std::size_t& used, std::size_t room, const Site& site, u32 kind) {
    u8* code = reinterpret_cast<u8*>(Game(site.address));
    if (std::memcmp(code, site.code.data(), site.code.size()) != 0) {
        return false;
    }
    Emitter e;
    // At the writer's return the caller-saved registers are free: the arguments set first.
    switch (site.context) { // mov rdi, <context>
    case Rbx:
        e.Bytes({0x48, 0x89, 0xdf});
        break;
    case R14:
        e.Bytes({0x4c, 0x89, 0xf7});
        break;
    default:
        return false;
    }
    switch (site.state) { // mov rsi, <state>
    case Rbx:
        e.Bytes({0x48, 0x89, 0xde});
        break;
    case R12:
        e.Bytes({0x4c, 0x89, 0xe6});
        break;
    case R13:
        e.Bytes({0x4c, 0x89, 0xee});
        break;
    case R15:
        e.Bytes({0x4c, 0x89, 0xfe});
        break;
    default:
        return false;
    }
    e.Bytes({0xba, u8(kind), 0, 0, 0}); // mov edx, kind
    e.SaveVolatile();
    e.CallAbs(reinterpret_cast<const void*>(&OnDraw));
    e.RestoreVolatile();
    for (const u8 b : site.code) {
        e.Bytes({b});
    }
    e.JmpAbs(Game(site.address) + site.code.size());
    if (e.overflow || used + e.size > room) {
        return false;
    }
    u8* stub = stubs + used;
    const s64 rel = s64(reinterpret_cast<u64>(stub)) - s64(Game(site.address) + 5);
    if (rel != s32(rel) || !Unlock(Game(site.address), site.code.size())) {
        return false;
    }
    std::memcpy(stub, e.code.data(), e.size);
    used += (e.size + 15) & ~std::size_t(15);
    const s32 rel32 = s32(rel);
    code[0] = 0xe9; // jmp stub
    std::memcpy(code + 1, &rel32, 4);
    for (std::size_t i = 5; i < site.code.size(); ++i) {
        code[i] = 0xcc;
    }
    return true;
}

void LogCounts() {
    const auto percent = [](u64 part, u64 whole) {
        return whole ? 100.0 * double(part) / double(whole) : 0.0;
    };
    u64 hooked = 0;
    for (auto& n : calls) {
        hooked += n.exchange(0, std::memory_order_relaxed);
    }
    std::string fields;
    for (u32 f = 0; f < FieldCount; ++f) {
        const u64 checked = counts.same[f] + counts.differ[f];
        if (checked == 0) {
            continue;
        }
        fields +=
            fmt::format("{}{} {:.2f}% of {} ({} differ)", fields.empty() ? "" : "; ", FieldNames[f],
                        percent(counts.same[f], checked), checked, counts.differ[f]);
    }
    if (!check_fields) {
        LOG_INFO(Render,
                 "GX pipelines: {:.1f}% of draws took their remembered pipeline; {} worked out "
                 "again to check, {} different{}",
                 percent(counts.reused, counts.draws), counts.spot_checks, counts.spot_differed,
                 reuse_stopped ? " (reuse stopped)" : "");
        counts = {};
        return;
    }
    LOG_INFO(Render,
             "GX draw capture: {} GX draw calls, {} draw packets run, {:.1f}% with a capture "
             "({} replaced by a newer draw, {} tessellated, {} with a geometry shader, {} "
             "dropped); agreeing: {}",
             hooked, counts.draws, percent(counts.captured, counts.draws), counts.stale,
             counts.tess, counts.gs, captures_dropped.exchange(0, std::memory_order_relaxed),
             fields.empty() ? "-" : fields);
    std::string memo;
    for (size_t k = 0; k < MemoKeys.size(); ++k) {
        const u64 known = counts.memo_same[k] + counts.memo_differ[k];
        memo += fmt::format(
            "{}{}: {:.2f}% of draws known ({:.4f}% of them a different "
            "pipeline, {} draws), {} keys new",
            memo.empty() ? "" : "; ", MemoKeys[k], percent(known, known + counts.memo_new[k]),
            percent(counts.memo_differ[k], known), counts.memo_differ[k], counts.memo_new[k]);
    }
    if (check_fields) {
        LOG_INFO(Render, "GX pipeline memo (by the engine's objects): {}", memo);
    }
    if (use_pipelines || reuse_stopped) {
        LOG_INFO(Render,
                 "GX pipelines: {:.1f}% of draws took their remembered pipeline; {} worked out "
                 "again to check, {} different{}",
                 percent(counts.reused, counts.draws), counts.spot_checks, counts.spot_differed,
                 reuse_stopped ? " (reuse stopped)" : "");
    }
    counts = {};
}

} // namespace

void Install() {
    check_fields = EmulatorSettings.IsBbGxObserve();
    use_pipelines = EmulatorSettings.IsBbGxPipelines();
    if (!check_fields && !use_pipelines) {
        return;
    }
    auto* param_sfo = Common::Singleton<PSF>::Instance();
    const auto version = param_sfo->GetString("APP_VER").value_or("");
    if (!Common::BbTuning::IsBloodborne(MemoryPatcher::g_game_serial) || version != "01.09" ||
        MemoryPatcher::g_eboot_name != "eboot.bin" || !MemoryPatcher::g_eboot_address) {
        LOG_INFO(Loader, "GX draw capture: not checked (Bloodborne 1.09 only)");
        return;
    }
    image_base = MemoryPatcher::g_eboot_address;
    if (MemoryPatcher::g_eboot_image_size < DefaultBlend + 4 - ImageBase) {
        LOG_WARNING(Loader, "GX draw capture: not checked (not Bloodborne 1.09's image)");
        return;
    }
    constexpr std::size_t Room = 4096;
    auto* stubs = static_cast<u8*>(
        AllocatePatchTrampolineSpace(reinterpret_cast<const void*>(image_base), Room));
    if (!stubs) {
        LOG_WARNING(Loader, "GX draw capture: not checked (no room for its hooks)");
        return;
    }
    // All six or none: a draw method left out would read as draws without a capture.
    for (const Site& site : Sites) {
        if (std::memcmp(reinterpret_cast<const void*>(Game(site.address)), site.code.data(),
                        site.code.size()) != 0) {
            LOG_WARNING(Loader, "GX draw capture: not checked ({} is not the expected code)",
                        site.name);
            return;
        }
    }
    std::size_t used = 0;
    for (u32 kind = 0; kind < Sites.size(); ++kind) {
        if (!Hook(stubs, used, Room, Sites[kind], kind)) {
            LOG_ERROR(Loader, "GX draw capture: the hook of {} did not fit; {} of 6 hooked",
                      Sites[kind].name, kind);
            return; // the ones in place only feed captures nothing reads
        }
    }
    hooked.store(true, std::memory_order_release);
    LOG_INFO(Loader, "GX draws: GX's six draw methods hooked;{}{} (results every 10 s)",
             check_fields ? " the command thread checks its draws against them" : "",
             use_pipelines ? " draws take the pipeline their objects gave before" : "");
}

bool Observing() {
    return hooked.load(std::memory_order_relaxed);
}

void CheckDraw(u64 guest_end, std::span<const u32, 2> tail, const AmdGpu::Regs& regs) {
    const auto now = std::chrono::steady_clock::now();
    if (counts_start == std::chrono::steady_clock::time_point{}) {
        counts_start = now;
    } else if (now - counts_start >= std::chrono::seconds(10)) {
        LogCounts();
        counts_start = now;
    }
    ++counts.draws;
    current_valid = false;
    Capture c;
    {
        Shard& shard = ShardOf(guest_end);
        std::scoped_lock lock{shard.mutex};
        const auto it = shard.captures.find(guest_end);
        if (it == shard.captures.end()) {
            return; // not a GX draw (YEBIS, Scaleform, the engine's own passes)
        }
        c = it->second;
        shard.captures.erase(it);
    }
    ++counts.captured;
    if (c.tail[0] != tail[0] || c.tail[1] != tail[1]) {
        ++counts.stale;
        return;
    }
    if (c.tess) {
        ++counts.tess;
    }
    if (c.gs) {
        ++counts.gs;
    }
    current_valid = true;
    current_memo = c.memo;
    if (!check_fields) {
        return;
    }
    std::array<int, FieldCount> result{}; // 1 same, -1 differ, 0 not checked
    const auto check = [&](Field f, bool same) {
        result[f] = same ? 1 : -1;
        ++(same ? counts.same : counts.differ)[f];
    };
    // With a geometry shader the vertex stage runs GX's geometry shader's copy program, and GX's
    // vertex shader runs as the export stage.
    const u64 vs = u64((c.gs && !c.tess ? regs.es_program : regs.vs_program).address) << 8;
    const u64 ps = u64(regs.ps_program.address) << 8;
    if (!(c.gs && c.tess)) {
        check(Vs, c.vs == vs);
    }
    if (c.has_ps) {
        check(Ps, c.ps == ps);
    }
    if (c.prim_known) {
        check(Prim, c.prim == static_cast<u32>(regs.primitive_type));
    }
    if (Indexed(c.kind) && c.ib_known) {
        check(Index, c.ib == regs.index_base_address.Address());
        check(IndexType, c.ib_type == static_cast<u32>(regs.index_buffer_type.index_type));
    }
    check(TargetMask, c.target_mask == regs.color_target_mask.raw);
    if (differences_logged < 16 && std::ranges::find(result, -1) != result.end()) {
        ++differences_logged;
        LOG_INFO(Render,
                 "GX draw capture differs ({} at {:#x}): vs {:#x}/{:#x} ps {:#x}/{:#x}{} prim "
                 "{}/{} index {:#x}/{:#x} size {}/{} mask {:#x}/{:#x} (capture/registers)",
                 Sites[c.kind].name, guest_end, c.vs, vs, c.ps, ps, c.has_ps ? "" : " (none)",
                 c.prim, static_cast<u32>(regs.primitive_type), c.ib,
                 regs.index_base_address.Address(), c.ib_type,
                 static_cast<u32>(regs.index_buffer_type.index_type), c.target_mask,
                 regs.color_target_mask.raw);
    }
}

const void* RememberedPipeline(u64 generation, u64 targets) {
    if (!use_pipelines || !current_valid) {
        return nullptr;
    }
    current_targets = targets;
    if (generation != memo_generation) {
        for (auto& memo : memos) {
            memo.clear();
        }
        memo_generation = generation;
        return nullptr;
    }
    const u64 key = KeyFor(UseKey);
    const auto it = memos[UseKey].find(key);
    if (it == memos[UseKey].end() || unreliable_keys.contains(key)) {
        return nullptr;
    }
    // Every 256th one is worked out again and compared (NotePipeline): the caller gets null.
    if (++spot_counter % SpotEvery == 0) {
        ++counts.spot_checks;
        spot_pending = it->second;
        return nullptr;
    }
    current_valid = false;
    ++counts.reused;
    return it->second;
}

const void* NotePipeline(const void* pipeline, u64 targets) {
    if (!current_valid || !pipeline) {
        spot_pending = nullptr;
        return nullptr;
    }
    current_valid = false;
    current_targets = targets;
    const void* differed = nullptr;
    if (spot_pending) {
        if (spot_pending != pipeline) {
            differed = spot_pending;
            ++counts.spot_differed;
            ++spot_differences;
            // This key told two pipelines apart: never reused again. Past a few such keys the
            // objects aren't telling pipelines reliably here: reuse stops.
            unreliable_keys.insert(KeyFor(UseKey));
            if (spot_differences >= MaxDiffers) {
                use_pipelines = false;
                reuse_stopped = true;
                LOG_ERROR(Render,
                          "GX pipelines: {} draws' objects told a different pipeline than "
                          "their registers; pipelines are worked out from the registers "
                          "again",
                          spot_differences);
            } else {
                LOG_WARNING(Render,
                            "GX pipelines: a draw's objects told a different pipeline than "
                            "its registers ({} of {} allowed); that key is worked out from "
                            "the registers from now on",
                            spot_differences, MaxDiffers);
            }
        }
        spot_pending = nullptr;
    }
    // Only the key in use, unless the keys are being compared.
    for (size_t k = check_fields ? 0 : UseKey; k < (check_fields ? memos.size() : UseKey + 1);
         ++k) {
        auto& memo = memos[k];
        if (memo.size() >= MemoLimit) {
            memo.clear();
        }
        const auto [it, added] = memo.try_emplace(KeyFor(k), pipeline);
        if (added) {
            ++counts.memo_new[k];
        } else if (it->second == pipeline) {
            ++counts.memo_same[k];
        } else {
            ++counts.memo_differ[k];
            it->second = pipeline;
        }
    }
    return differed;
}

} // namespace Core::BbGxDraws
