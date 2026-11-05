// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"

namespace Vulkan {

class Instance;
class Scheduler;

struct StagingBufferRef {
    VideoCore::Buffer* buffer{};
    u64 offset{};
    u64 size{};
    u8* mapped{};
    VideoCore::MemoryType type{};

    void Flush() const noexcept {
        buffer->Flush(offset, size);
    }

    void Invalidate() const noexcept {
        buffer->Invalidate(offset, size);
    }
};

class StagingBufferPool {
public:
    explicit StagingBufferPool(const Instance& instance, Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    /// Requests size bytes of memory of the given type.
    [[nodiscard]] StagingBufferRef Request(u64 size, VideoCore::MemoryType type, u64 alignment = 0,
                                           bool deferred = false, bool unsynchronized = false);

    /// Releases a deferred allocation. It becomes reusable once the current tick completes.
    void FreeDeferred(const StagingBufferRef& ref);

    /// Stage uploads in VRAM (setting): size bytes in a fixed area of graphics card memory the
    /// CPU writes through Resizable BAR, or nothing (setting off, no Resizable BAR, an integrated
    /// GPU, the area full of uploads the GPU hasn't copied out yet, VRAM tight): the caller
    /// stages in system RAM as before.
    [[nodiscard]] std::optional<StagingBufferRef> RequestVram(u64 size, u64 alignment = 0);

    /// Releases idle memory.
    void TickFrame();

    /// VRAM cleanup under pressure: frees the graphics card memory blocks and buffers (scratch
    /// for detiling and copies) unused for about a second whose GPU work is done.
    struct ReleasedMemory {
        u64 bytes{};
        u32 ring_blocks{};
        u32 large_buffers{};
    };
    /// emergency (an allocation failed): everything the GPU is done with, no floor or idle time.
    ReleasedMemory ReleaseIdleDeviceLocal(bool emergency = false);

    /// Graphics card memory the pool holds now.
    [[nodiscard]] u64 DeviceLocalBytes() const;

private:
    struct Block {
        std::unique_ptr<VideoCore::StreamBuffer> buffer;
        u64 last_used_frame;
    };

    struct Ring {
        std::vector<Block> blocks;
        size_t current = 0;
    };

    struct LargeBuffer {
        std::unique_ptr<VideoCore::Buffer> buffer;
        u64 tick;
        u64 last_used_frame;
        bool held;
    };

    static constexpr size_t NUM_TYPES = 3;

    StagingBufferRef RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                     VideoCore::MemoryType type, bool unsynchronized);
    StagingBufferRef RequestLarge(u64 size, VideoCore::MemoryType type, bool deferred,
                                  bool unsynchronized);

    void TrimRing(Ring& ring, size_t min_blocks);

    void CountAllocation(u64 bytes, std::chrono::steady_clock::time_point start);
    void TrimLarge(std::vector<LargeBuffer>& cache);

    void PublishStats() const;

    const Instance& instance;
    Scheduler& scheduler;

    /// Stage uploads in VRAM: up to VRAM_RING_BLOCKS blocks, made as uploads first need them and
    /// kept while the setting is on (released when it goes off and under VRAM pressure).
    Ring vram_ring;
    bool vram_capable = false;       ///< Resizable BAR on a dedicated graphics card
    u64 vram_paused_until_frame = 0; ///< after a release under VRAM pressure
    u64 vram_uploads = 0;            ///< bytes staged there since the last log line
    u64 vram_fallbacks = 0;          ///< requests it couldn't take since the last log line
    std::chrono::steady_clock::time_point vram_log_time{};
    void ReleaseVramRing(bool only_idle, ReleasedMemory* released);

    std::array<Ring, NUM_TYPES> rings;
    std::array<std::vector<LargeBuffer>, NUM_TYPES> large_caches;
    u64 frame = 0;
    size_t min_upload_blocks = 0;
    /// The memory texture uploads are staged in (BufferCache::ObtainBufferForImage): its ring
    /// keeps min_upload_blocks.
    VideoCore::MemoryType TextureStagingType() const;
    /// Blocks each ring keeps (TrimRing): min_upload_blocks for texture staging, a few for
    /// buffer uploads.
    size_t KeptBlocks(VideoCore::MemoryType type) const;
    std::chrono::steady_clock::time_point frame_time{};
};

} // namespace Vulkan
