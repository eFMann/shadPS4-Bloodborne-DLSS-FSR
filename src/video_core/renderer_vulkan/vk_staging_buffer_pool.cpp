// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>

#include <SDL3/SDL_cpuinfo.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bb_tuning.h"
#include "common/elf_info.h"
#include "common/guest_stats.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// "Frames" here advance at most every 16 ms (TickFrame runs once per guest submission, ~1600
// a second in Bloodborne, which freed idle staging after ~0.2 s: every streaming burst then
// allocated fresh memory): ~5 s and ~2 s.
constexpr u64 RING_IDLE_FRAMES = 300;
constexpr u64 LARGE_IDLE_FRAMES = 120;
constexpr u64 LARGE_IDLE_FRAMES_STREAMING = 600; // ~10 s
constexpr auto FRAME_PERIOD = std::chrono::milliseconds(16);
// Upload blocks (16 MiB each) allocated at startup and always kept: streaming a new area uploads
// 200-450 MiB in one frame, and allocating pinned memory mid-game once took 176 ms for 144 MiB
// (and copies into fresh pages are slower). Large dedicated uploads are kept ~10 s.
// Entering a big area (the DLC) still needed ~1 GiB: 67 more blocks in that one frame, 66 ms.
// So with 16 GB of RAM or more, 1 GiB is kept ready (384 MiB below that).
constexpr size_t MIN_UPLOAD_BLOCKS_SMALL = 24;
constexpr size_t MIN_UPLOAD_BLOCKS_LARGE = 64;
// Buffer uploads always stage in uncached memory: with texture staging in cached memory, that
// ring kept nothing and a burst allocated a block again (16 MiB, 20-24 ms of pinned memory).
constexpr size_t MIN_BUFFER_UPLOAD_BLOCKS = 8;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;
// Graphics card memory released under VRAM pressure once unused this long (~3 s): taking it back
// costs an allocation, keeping it cost textures the game was using. Streaming keeps ~300 MiB of
// it busy, so released below a floor it was allocated again within seconds (3.5 of 3.9 GiB
// released in a session): the floor stays.
constexpr u64 RELEASE_IDLE_FRAMES = 180;
constexpr size_t RELEASE_FLOOR_BLOCKS = 12;
// Stage uploads in VRAM: 8 blocks, 128 MiB. A traversal burst stages 50-160 MiB, and blocks free
// again within a frame or two once the GPU has copied out of them; entering a big area (300+ MiB)
// overflows to system RAM. After a release under VRAM pressure: ~10 s without.
constexpr size_t VRAM_RING_BLOCKS = 8;
constexpr u64 VRAM_PAUSE_FRAMES = 600;
// Resizable BAR / Smart Access Memory: the CPU-visible part of VRAM covers (nearly) all of it,
// not the classic 256 MiB window the driver and the draw data may need.
constexpr u64 REBAR_MIN_VISIBLE = 1024_MB;

void CountDeviceLocalAllocation(MemoryType type, u64 bytes) {
    if (type != MemoryType::DeviceLocal) {
        return;
    }
    namespace GS = Common::GuestStats;
    GS::scratch_allocations.fetch_add(1, std::memory_order_relaxed);
    GS::scratch_allocation_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    {
        // Stage uploads in VRAM: only with Resizable BAR on a dedicated card (an integrated
        // GPU's "VRAM" is system RAM: nothing to save).
        const auto props = instance.GetPhysicalDevice().getMemoryProperties();
        constexpr auto bar_flags =
            vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible;
        u64 visible = 0;
        for (u32 i = 0; i < props.memoryTypeCount; ++i) {
            if ((props.memoryTypes[i].propertyFlags & bar_flags) == bar_flags) {
                visible =
                    std::max<u64>(visible, props.memoryHeaps[props.memoryTypes[i].heapIndex].size);
            }
        }
        vram_capable = !instance.IsIntegrated() && visible > REBAR_MIN_VISIBLE;
        LOG_INFO(Render_Vulkan, "Stage uploads in VRAM: {} ({} MiB CPU-visible VRAM{})",
                 vram_capable ? "possible" : "not possible", visible >> 20,
                 instance.IsIntegrated() ? ", integrated GPU" : "");
    }
    // Bloodborne threaded renderer with streaming optimisations only.
    if (!Common::BbTuning::IsBloodborne(Common::ElfInfo::Instance().GameSerial()) ||
        !EmulatorSettings.IsThreadedRenderer() || !EmulatorSettings.IsBbStreaming()) {
        return;
    }
    // Allocated and touched now, so the pages are committed before the game streams.
    min_upload_blocks =
        SDL_GetSystemRAM() >= 15 * 1024 ? MIN_UPLOAD_BLOCKS_LARGE : MIN_UPLOAD_BLOCKS_SMALL;
    // In the memory texture uploads use: with cached texture staging the kept blocks were in
    // the other kind, and every burst after an idle spell allocated 150-220 MiB again (5-8 ms).
    for (size_t type = 0; type < rings.size(); ++type) {
        const size_t blocks = KeptBlocks(MemoryType(type));
        auto& ring = rings[type];
        for (size_t i = 0; i < blocks; ++i) {
            auto& block = ring.blocks.emplace_back(Block{
                .buffer = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler,
                                                                    MemoryType(type), BLOCK_SIZE),
                .last_used_frame = 0,
            });
            if (!block.buffer->mapped_data.empty()) {
                std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
            }
        }
    }
}

size_t StagingBufferPool::KeptBlocks(MemoryType type) const {
    if (type == TextureStagingType()) {
        return min_upload_blocks;
    }
    return type == MemoryType::HostUncached && min_upload_blocks != 0 ? MIN_BUFFER_UPLOAD_BLOCKS
                                                                      : 0;
}

StagingBufferPool::~StagingBufferPool() = default;

MemoryType StagingBufferPool::TextureStagingType() const {
    return EmulatorSettings.IsThreadedRenderer() && EmulatorSettings.IsBbCachedTextureStaging()
               ? MemoryType::HostCached
               : MemoryType::HostUncached;
}

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment, bool deferred,
                                            bool unsynchronized) {
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred, unsynchronized);
    }
    return RequestFromRing(ring, size, alignment, type, unsynchronized);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type, bool unsynchronized) {
    Block* best{};
    u64 best_offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        if (!ring.blocks.empty() && unsynchronized) {
            best = &ring.blocks[(ring.current + 1) % ring.blocks.size()];
            best_offset = 0;
        } else {
            const auto start = std::chrono::steady_clock::now();
            best = &ring.blocks.emplace_back(Block{
                .buffer = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type,
                                                                    BLOCK_SIZE),
                .last_used_frame = frame,
            });
            CountAllocation(BLOCK_SIZE, start);
            CountDeviceLocalAllocation(type, BLOCK_SIZE);
            ring.current = ring.blocks.size() - 1;
            best_offset = *best->buffer->Reserve(size, alignment, false);
        }
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred,
                                                 bool unsynchronized) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            (!scheduler.IsFree(entry.tick) && !unsynchronized)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        const auto start = std::chrono::steady_clock::now();
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
        CountAllocation(RoundAllocationSize(size), start);
        CountDeviceLocalAllocation(type, RoundAllocationSize(size));
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

std::optional<StagingBufferRef> StagingBufferPool::RequestVram(u64 size, u64 alignment) {
    if (!vram_capable || size > BLOCK_SIZE || frame < vram_paused_until_frame ||
        !EmulatorSettings.IsBbVramUploadStaging()) {
        return std::nullopt;
    }
    Ring& ring = vram_ring;
    Block* found{};
    u64 offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto reserved = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            found = block;
            offset = *reserved;
            break;
        }
    }
    if (!found) {
        if (ring.blocks.size() >= VRAM_RING_BLOCKS) {
            // Full of uploads the GPU hasn't copied out yet: system RAM, as before.
            ++vram_fallbacks;
            return std::nullopt;
        }
        const auto start = std::chrono::steady_clock::now();
        auto buffer = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler,
                                                                MemoryType::Stream, BLOCK_SIZE);
        if (buffer->mapped_data.empty()) {
            ++vram_fallbacks;
            return std::nullopt;
        }
        found = &ring.blocks.emplace_back(Block{
            .buffer = std::move(buffer),
            .last_used_frame = frame,
        });
        CountAllocation(BLOCK_SIZE, start);
        ring.current = ring.blocks.size() - 1;
        const auto reserved = found->buffer->Reserve(size, alignment, false);
        if (!reserved) {
            ++vram_fallbacks;
            return std::nullopt;
        }
        offset = *reserved;
    }
    found->last_used_frame = frame;
    vram_uploads += size;
    return StagingBufferRef{
        .buffer = found->buffer.get(),
        .offset = offset,
        .size = size,
        .mapped = found->buffer->mapped_data.data() + offset,
        .type = MemoryType::Stream,
    };
}

void StagingBufferPool::ReleaseVramRing(bool only_idle, ReleasedMemory* released) {
    auto& blocks = vram_ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        if (!scheduler.IsFree(block.buffer->LastTick()) ||
            (only_idle && frame - block.last_used_frame < RELEASE_IDLE_FRAMES)) {
            continue;
        }
        if (released) {
            released->bytes += block.buffer->SizeBytes();
            ++released->ring_blocks;
        }
        blocks.erase(blocks.begin() + i);
    }
    vram_ring.current = 0;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::CountAllocation(u64 bytes, std::chrono::steady_clock::time_point start) {
    namespace GS = Common::GuestStats;
    GS::staging_allocations.fetch_add(1, std::memory_order_relaxed);
    GS::staging_allocation_bytes.fetch_add(bytes, std::memory_order_relaxed);
    GS::staging_allocation_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - start)
                                            .count(),
                                        std::memory_order_relaxed);
}

void StagingBufferPool::TickFrame() {
    // Streaming optimisations: frames advance at most every 16 ms and upload blocks are kept.
    // Off: the original, one frame per guest submission.
    const bool streaming = Common::BbTuning::Streaming();
    if (streaming) {
        const auto now = std::chrono::steady_clock::now();
        if (now - frame_time < FRAME_PERIOD) {
            return;
        }
        frame_time = now;
    }
    ++frame;
    for (size_t type = 0; type < rings.size(); ++type) {
        TrimRing(rings[type], streaming ? KeptBlocks(MemoryType(type)) : size_t{0});
    }
    for (auto& cache : large_caches) {
        TrimLarge(cache);
    }
    // Stage uploads in VRAM turned off: its blocks go once the GPU is done with them.
    if (!vram_ring.blocks.empty() && !EmulatorSettings.IsBbVramUploadStaging()) {
        const size_t before = vram_ring.blocks.size();
        ReleaseVramRing(false, nullptr);
        if (vram_ring.blocks.size() != before) {
            LOG_INFO(Render_Vulkan, "Stage uploads in VRAM: off, released {} MiB",
                     (before - vram_ring.blocks.size()) * (BLOCK_SIZE >> 20));
        }
    }
    if ((vram_uploads != 0 || vram_fallbacks != 0) &&
        std::chrono::steady_clock::now() - vram_log_time >= std::chrono::seconds(10)) {
        vram_log_time = std::chrono::steady_clock::now();
        LOG_INFO(Render_Vulkan,
                 "Stage uploads in VRAM (10 s): {:.1f} MiB staged in VRAM, {} uploads to system "
                 "RAM (area full), {} MiB held",
                 static_cast<double>(vram_uploads) / 1048576.0, vram_fallbacks,
                 vram_ring.blocks.size() * (BLOCK_SIZE >> 20));
        vram_uploads = 0;
        vram_fallbacks = 0;
    }
    PublishStats();
}

StagingBufferPool::ReleasedMemory StagingBufferPool::ReleaseIdleDeviceLocal(bool emergency) {
    ReleasedMemory released{};
    const size_t floor_blocks = emergency ? 0 : RELEASE_FLOOR_BLOCKS;
    const u64 idle_frames = emergency ? 0 : RELEASE_IDLE_FRAMES;
    Ring& ring = rings[u32(MemoryType::DeviceLocal)];
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        if (blocks.size() <= floor_blocks) {
            break;
        }
        const Block& block = blocks[i];
        if (frame - block.last_used_frame < idle_frames ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        released.bytes += block.buffer->SizeBytes();
        ++released.ring_blocks;
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
    std::erase_if(large_caches[u32(MemoryType::DeviceLocal)], [&](const LargeBuffer& entry) {
        if (entry.held || frame - entry.last_used_frame < idle_frames ||
            !scheduler.IsFree(entry.tick)) {
            return false;
        }
        released.bytes += entry.buffer->SizeBytes();
        ++released.large_buffers;
        return true;
    });
    // Stage uploads in VRAM: under pressure its blocks go too, and it stays off for a while
    // (system RAM meanwhile) so the next burst doesn't take the memory straight back.
    if (!vram_ring.blocks.empty()) {
        const u64 before = released.bytes;
        ReleaseVramRing(!emergency, &released);
        if (released.bytes != before) {
            vram_paused_until_frame = frame + VRAM_PAUSE_FRAMES;
            LOG_INFO(Render_Vulkan, "Stage uploads in VRAM: released {} MiB (VRAM {}), paused",
                     (released.bytes - before) >> 20, emergency ? "full" : "under pressure");
        }
    }
    return released;
}

u64 StagingBufferPool::DeviceLocalBytes() const {
    u64 bytes = vram_ring.blocks.size() * BLOCK_SIZE;
    for (const Block& block : rings[u32(MemoryType::DeviceLocal)].blocks) {
        bytes += block.buffer->SizeBytes();
    }
    for (const LargeBuffer& entry : large_caches[u32(MemoryType::DeviceLocal)]) {
        bytes += entry.buffer->SizeBytes();
    }
    return bytes;
}

void StagingBufferPool::TrimRing(Ring& ring, size_t min_blocks) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        if (blocks.size() <= min_blocks) {
            break;
        }
        const Block& block = blocks[i];
        if (frame - block.last_used_frame < RING_IDLE_FRAMES ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache) {
    std::erase_if(cache, [this](const LargeBuffer& entry) {
        const u64 idle =
            Common::BbTuning::Streaming() ? LARGE_IDLE_FRAMES_STREAMING : LARGE_IDLE_FRAMES;
        return !entry.held && frame - entry.last_used_frame >= idle && scheduler.IsFree(entry.tick);
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
