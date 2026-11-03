// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include "common/assert.h"
#include "common/copy_pool.h"
#include "common/crash_crumbs.h"
#include "common/debug.h"
#include "common/guest_stats.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_bb_stage_census.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace Vulkan {

std::mutex Scheduler::submit_mutex;
std::mutex Scheduler::present_mutex;
std::mutex Scheduler::queue_ops_mutex;
bool Scheduler::deferring_queue_ops = false;
std::vector<Common::UniqueFunction<void>> Scheduler::deferred_queue_ops;

void Scheduler::QueueOp(Common::UniqueFunction<void>&& op) {
    std::scoped_lock lk{queue_ops_mutex};
    if (deferring_queue_ops) {
        deferred_queue_ops.push_back(std::move(op));
        Common::GuestStats::queue_ops_behind_present.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    op();
}

void Scheduler::BeginDeferringQueueOps() {
    std::scoped_lock lk{queue_ops_mutex};
    deferring_queue_ops = true;
}

void Scheduler::EndDeferringQueueOps() {
    // submit_mutex first (the order QueueOp's callers take them in): everything else that uses
    // the queue or waits for the device to be idle holds it.
    std::scoped_lock lk{submit_mutex, queue_ops_mutex};
    for (auto& op : deferred_queue_ops) {
        op();
    }
    deferred_queue_ops.clear();
    deferring_queue_ops = false;
}

Scheduler::Scheduler(const Instance& instance)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore},
      tail_pool{instance, &work_semaphore} {
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    BeginSession();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    CatchUp();
    BB_STAGE_CENSUS("Scheduler::BeginRendering", Commands);
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;
    Record([render_state = render_state](vk::CommandBuffer cmdbuf) {
        RecordBeginRendering(cmdbuf, render_state);
    });
}

void Scheduler::RecordBeginRendering(vk::CommandBuffer cmdbuf, const RenderState& render_state) {
    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    cmdbuf.beginRendering(rendering_info);
}

void Scheduler::EndRendering() {
    BB_STAGE_CENSUS("Scheduler::EndRendering", Commands);
    CatchUp();
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
    if (pass_end_hook && !in_pass_end_hook) {
        in_pass_end_hook = true;
        pass_end_hook();
        in_pass_end_hook = false;
    }
}

void Scheduler::NoteSync(const std::source_location& where) const {
    const u64 key = (u64(where.line()) << 32) ^ std::hash<std::string_view>{}(where.file_name());
    std::scoped_lock lk{sync_stats_mutex};
    auto [it, is_new] = sync_sites.try_emplace(key);
    if (is_new) {
        std::string_view file{where.file_name()};
        if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
            file = file.substr(slash + 1);
        }
        it->second.first = fmt::format("{}:{}", file, where.line());
    }
    ++it->second.second;
}

std::string Scheduler::TakeRecordingStats() {
    std::vector<std::pair<std::string, u64>> sites;
    {
        std::scoped_lock lk{sync_stats_mutex};
        for (auto& [key, site] : sync_sites) {
            sites.push_back(site);
        }
        sync_sites.clear();
    }
    std::ranges::sort(sites, [](const auto& a, const auto& b) { return a.second > b.second; });
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    const s64 busy = recorder ? recorder->busy_ns.exchange(0, std::memory_order_relaxed) : 0;
    std::string out =
        fmt::format("recording thread busy {:.1f}%; ", seconds > 0 ? busy / seconds / 1e7 : 0.0);
    out += fmt::format("{} deferred, {} direct;",
                       deferred_records.exchange(0, std::memory_order_relaxed),
                       direct_records.exchange(0, std::memory_order_relaxed));
    for (size_t i = 0; i < std::min<size_t>(sites.size(), 6); ++i) {
        out += fmt::format(" {} x{}", sites[i].first, sites[i].second);
    }
    return out;
}

void Scheduler::EnableThreadedRecording() {
    {
        const auto props = instance.GetPhysicalDevice().getProperties();
        if (props.limits.timestampComputeAndGraphics) {
            timer_period_ns = props.limits.timestampPeriod;
            auto [result, pool] =
                instance.GetDevice().createQueryPoolUnique(vk::QueryPoolCreateInfo{
                    .queryType = vk::QueryType::eTimestamp,
                    .queryCount = TimerPairs * 2,
                });
            if (result == vk::Result::eSuccess) {
                timer_pool = std::move(pool);
            }
        }
    }
    if (!recorder) {
        recorder = std::make_unique<CommandRecorder>();
        recorder->SetCommandBuffer(sessions.back().primary);
        direct_mode = true;
    }
}

CommandRecorder::CommandRecorder() {
    thread = std::jthread([this](std::stop_token stop) { Run(stop); });
}

CommandRecorder::~CommandRecorder() {
    Sync();
    // Hand over an empty slot as the exit signal. head.wait() only returns once the value
    // changes, so the thread must be woken by a real new entry, not just a notify.
    const u64 at = head.load(std::memory_order_relaxed);
    ring[at % RingSize] = nullptr;
    head.store(at + 1, std::memory_order_seq_cst);
    head.notify_all();
    thread.join();
}

CommandRecorder::Chunk* CommandRecorder::AcquireChunk() {
    {
        std::scoped_lock lk{free_mutex};
        if (!free_chunks.empty()) {
            Chunk* chunk = free_chunks.back();
            free_chunks.pop_back();
            return chunk;
        }
    }
    return chunks.emplace_back(std::make_unique<Chunk>()).get();
}

void CommandRecorder::Kick(bool force) {
    if (!current || current->used == 0 || (!force && current->used < KickBytes)) {
        return;
    }
    const u64 at = head.load(std::memory_order_relaxed);
    for (u32 spins = 0; at - tail.load(std::memory_order_acquire) >= RingSize; ++spins) {
        if (spins < 4096) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    ring[at % RingSize] = current;
    current = nullptr;
    head.store(at + 1, std::memory_order_seq_cst);
    if (sleeping.load(std::memory_order_seq_cst)) {
        head.notify_one();
    }
}

void CommandRecorder::Sync(std::source_location where) {
    const auto start = std::chrono::steady_clock::now();
    const bool asleep = sleeping.load(std::memory_order_relaxed);
    const u64 bytes = current ? current->used : 0;
    Kick(true);
    const u64 target = head.load(std::memory_order_relaxed);
    const u64 chunks = target - tail.load(std::memory_order_acquire);
    for (u32 spins = 0; tail.load(std::memory_order_acquire) < target; ++spins) {
        if (spins >= 1024) {
            std::this_thread::yield();
        }
    }
    NoteSyncWait(where, (std::chrono::steady_clock::now() - start).count(), chunks, bytes,
                 asleep);
}

void CommandRecorder::NoteSyncWait(const std::source_location& where, s64 ns, u64 chunks,
                                   u64 bytes, bool asleep) {
    std::scoped_lock lk{sync_wait_mutex};
    ++sync_waits;
    sync_wait_ns += ns;
    sync_wait_max_ns = std::max(sync_wait_max_ns, ns);
    sync_chunks += chunks;
    sync_bytes += bytes;
    sync_asleep += asleep ? 1 : 0;
    auto site = std::ranges::find_if(sync_wait_sites, [&](const SyncSite& s) {
        return s.file == where.file_name() && s.line == where.line();
    });
    if (site == sync_wait_sites.end()) {
        sync_wait_sites.push_back({where.file_name(), where.line()});
        site = sync_wait_sites.end() - 1;
    }
    ++site->count;
    site->ns += ns;
    const auto now = std::chrono::steady_clock::now();
    if (sync_log_time == std::chrono::steady_clock::time_point{}) {
        sync_log_time = now;
        sync_busy_mark = busy_ns.load(std::memory_order_relaxed);
        return;
    }
    const double seconds = std::chrono::duration<double>(now - sync_log_time).count();
    if (seconds < 10.0) {
        return;
    }
    const s64 busy = busy_ns.load(std::memory_order_relaxed);
    std::ranges::sort(sync_wait_sites,
                      [](const SyncSite& a, const SyncSite& b) { return a.ns > b.ns; });
    std::string sites;
    for (size_t i = 0; i < std::min<size_t>(sync_wait_sites.size(), 4); ++i) {
        const auto& s = sync_wait_sites[i];
        std::string_view file{s.file};
        if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
            file.remove_prefix(slash + 1);
        }
        sites += fmt::format("{}{}:{} {}x {:.0f} us", i ? ", " : "", file, s.line, s.count,
                             double(s.ns) / 1e3 / double(std::max<u64>(s.count, 1)));
    }
    const double n = double(std::max<u64>(sync_waits, 1));
    const s64 mirror_ns = RecordingCosts::mirror_ns.exchange(0, std::memory_order_relaxed);
    const u64 mirror_commands =
        RecordingCosts::mirror_commands.exchange(0, std::memory_order_relaxed);
    const u64 mirror_batches = RecordingCosts::mirror_batches.exchange(0, std::memory_order_relaxed);
    const s64 upscaler_ns = RecordingCosts::upscaler_ns.exchange(0, std::memory_order_relaxed);
    const u64 upscaler_calls = RecordingCosts::upscaler_calls.exchange(0, std::memory_order_relaxed);
    std::array<u64, RecordingCosts::sync_submits.size()> sync_submits{};
    for (size_t i = 0; i < sync_submits.size(); ++i) {
        sync_submits[i] = RecordingCosts::sync_submits[i].exchange(0, std::memory_order_relaxed);
    }
    LOG_INFO(Render_Vulkan,
             "Recording thread jobs ({:.0f} s): velocity mirror replays {:.2f} ms a second "
             "({} batches, {:.0f} us and {:.0f} commands each), DLSS evaluations {:.2f} ms a "
             "second ({:.0f} us each); frame and guest submissions on the recording thread {}, "
             "waited for it {} (setting off {}, upload command buffer {}, several sessions {}, "
             "capture or GPU profile {}, recording thread far behind {})",
             seconds, double(mirror_ns) / 1e6 / seconds, mirror_batches,
             double(mirror_ns) / 1e3 / double(std::max<u64>(mirror_batches, 1)),
             double(mirror_commands) / double(std::max<u64>(mirror_batches, 1)),
             double(upscaler_ns) / 1e6 / seconds,
             double(upscaler_ns) / 1e3 / double(std::max<u64>(upscaler_calls, 1)),
             RecordingCosts::async_submits.exchange(0, std::memory_order_relaxed),
             sync_submits[0] + sync_submits[1] + sync_submits[2] + sync_submits[3] +
                 sync_submits[4],
             sync_submits[0], sync_submits[1], sync_submits[2], sync_submits[3],
             sync_submits[4]);
    LOG_INFO(Render_Vulkan,
             "Recording thread syncs ({:.0f} s): {} waits, avg {:.0f} us, max {:.0f} us, "
             "{:.2f} ms a second in all; at a wait {:.1f} chunks were still to record and "
             "{:.1f} KB not yet handed over, the recording thread asleep at {} of them; "
             "recording thread busy {:.0f}%; by caller (avg wait): {}",
             seconds, sync_waits, double(sync_wait_ns) / 1e3 / n, double(sync_wait_max_ns) / 1e3,
             double(sync_wait_ns) / 1e6 / seconds, double(sync_chunks) / n,
             double(sync_bytes) / 1024.0 / n, sync_asleep,
             double(busy - sync_busy_mark) / 1e7 / seconds, sites);
    sync_waits = sync_asleep = sync_chunks = sync_bytes = 0;
    sync_wait_ns = sync_wait_max_ns = 0;
    sync_wait_sites.clear();
    sync_busy_mark = busy;
    sync_log_time = now;
}

void CommandRecorder::Run(std::stop_token stop) {
    Common::SetCurrentThreadName("shadPS4:GpuCmdRecorder");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    u64 at = 0;
    while (true) {
        u64 available = head.load(std::memory_order_acquire);
        if (available == at) {
            // Spin briefly (commands follow each other while a frame is recorded), then sleep.
            const auto spin_until =
                std::chrono::steady_clock::now() + std::chrono::microseconds(100);
            for (u32 spins = 1; available == at; ++spins) {
                if (stop.stop_requested()) {
                    return;
                }
                // A pause between polls: without it this loop took a full core's issue slots,
                // half of them from the draw recorder when both run on one physical core.
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#else
                std::this_thread::yield();
#endif
                if ((spins & 255) == 0 && std::chrono::steady_clock::now() >= spin_until) {
                    sleeping.store(true, std::memory_order_seq_cst);
                    if (head.load(std::memory_order_seq_cst) == at && !stop.stop_requested()) {
                        head.wait(at, std::memory_order_seq_cst);
                    }
                    sleeping.store(false, std::memory_order_relaxed);
                }
                available = head.load(std::memory_order_acquire);
            }
        }
        Chunk* chunk = ring[at % RingSize];
        if (!chunk) {
            return; // shutdown
        }
        const auto start = std::chrono::steady_clock::now();
        for (size_t offset = 0; offset < chunk->used;) {
            auto* entry = reinterpret_cast<Entry*>(chunk->data + offset);
            entry->run(entry + 1, cmdbuf);
            offset += entry->size;
        }
        busy_ns.fetch_add((std::chrono::steady_clock::now() - start).count(),
                          std::memory_order_relaxed);
        chunk->used = 0;
        {
            std::scoped_lock lk{free_mutex};
            free_chunks.push_back(chunk);
        }
        ++at;
        tail.store(at, std::memory_order_release);
    }
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    CatchUp();
    // The command pool is shared with the recording thread's command buffer.
    if (recorder && !direct_mode) {
        SyncRecorder();
    }
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    upload_cmdbuf = command_pool.Commit();
    Check(upload_cmdbuf.begin(begin_info));
    return upload_cmdbuf;
}

void Scheduler::Flush(SubmitInfo& info) {
    CatchUp();
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    CatchUp();
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish(std::source_location where) {
    CatchUp();
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick, where);
}

void Scheduler::Wait(u64 tick, std::source_location where) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    work_semaphore.Wait(tick, where);
}

void Scheduler::PopPendingOperations() {
    std::unique_lock lk(pending_ops_mutex);
    work_semaphore.Refresh();
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
}

void Scheduler::BeginSession() {
    if (recorder) {
        SyncRecorder();
    }
    EndSession();

    auto& session = sessions.emplace_back();

    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    session.primary = command_pool.Commit();
    Check(session.primary.begin(begin_info));
    if (timer_pool && gpu_timing && !timer_open) {
        session.primary.resetQueryPool(*timer_pool, timer_slot * 2, 2);
        session.primary.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *timer_pool,
                                       timer_slot * 2);
        timer_open = true;
    }
    if (recorder) {
        recorder->SetCommandBuffer(session.primary);
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::EndSession() {
    if (sessions.empty()) {
        return;
    }

    if (on_session) {
        on_session();
    }

    const auto& session = sessions.back();
    if (session.upload) {
        Check(session.upload.end());
    }

    EndRendering();
    Check(session.primary.end());
    if (session.tail) {
        Check(session.tail.end());
    }
}

namespace {
thread_local std::array<u64, Scheduler::SubmitSteps.size()> last_submit_steps{};

// Stutter detail: times the steps of one submission (no clock reads while it is off).
class SubmitStepTimer {
public:
    SubmitStepTimer() : on{Common::GuestStats::detail_timing.load(std::memory_order_relaxed)} {
        last_submit_steps.fill(0);
        if (on) {
            lap = std::chrono::steady_clock::now();
        }
    }
    void Step(size_t index) {
        if (!on) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        last_submit_steps[index] += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - lap).count());
        lap = now;
    }

private:
    bool on;
    std::chrono::steady_clock::time_point lap{};
};
} // namespace

const std::array<u64, Scheduler::SubmitSteps.size()>& Scheduler::LastSubmitSteps() noexcept {
    return last_submit_steps;
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    Common::Crumbs::Add("GPU submit (tick)", CurrentTick());
    SubmitStepTimer steps;
    if (recorder) {
        SyncRecorder();
    }
    steps.Step(0);
    {
        // Background texture copies this submission reads (all recorded by now) must be done.
        const auto start = std::chrono::steady_clock::now();
        if (Common::CopyPool::WaitAsync()) {
            Common::GuestStats::async_copy_wait_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count(),
                std::memory_order_relaxed);
        }
    }
    steps.Step(1);
    std::scoped_lock lk{submit_mutex};
    steps.Step(2);
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }
    steps.Step(3);

    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    if (timer_pool && timer_open && !sessions.empty()) {
        EndRendering();
        sessions.back().primary.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                               *timer_pool, timer_slot * 2 + 1);
        if (pending_timings.size() >= TimerPairs - 1) {
            pending_timings.pop_front(); // never read: its slot is about to be reused
        }
        pending_timings.push_back({signal_value, timer_slot});
        timer_slot = (timer_slot + 1) % TimerPairs;
        timer_open = false;
    }

    EndSession();
    steps.Step(4);

    std::vector<vk::CommandBuffer> cmd_buffers;
    cmd_buffers.reserve(sessions.size() * 2);

    for (const auto& session : sessions) {
        if (session.upload) {
            cmd_buffers.push_back(session.upload);
        }
        cmd_buffers.push_back(session.primary);
        if (session.tail) {
            cmd_buffers.push_back(session.tail);
        }
    }
    sessions.clear();

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    // A copy of the submission: it may run after this returns (Keep rendering while presenting).
    QueueOp([queue = instance.GetGraphicsQueue(), info, cmd_buffers = std::move(cmd_buffers)] {
        QueueSubmit(queue, info, cmd_buffers);
    });
    NoteSubmitted(signal_value);
    steps.Step(5);

    work_semaphore.Refresh();
    CollectGpuTiming();
    steps.Step(6);
    BeginSession();
    steps.Step(7);

    // Apply pending operations
    PopPendingOperations();
    steps.Step(8);
}

void Scheduler::QueueSubmit(vk::Queue queue, const SubmitInfo& info,
                            std::vector<vk::CommandBuffer> cmd_buffers) {
    {
        std::array<vk::PipelineStageFlags, std::tuple_size_v<decltype(info.wait_semas)>>
            wait_stage_masks;
        wait_stage_masks.fill(vk::PipelineStageFlagBits::eAllCommands);

        const vk::TimelineSemaphoreSubmitInfo timeline_si = {
            .waitSemaphoreValueCount = info.num_wait_semas,
            .pWaitSemaphoreValues = info.wait_ticks.data(),
            .signalSemaphoreValueCount = info.num_signal_semas,
            .pSignalSemaphoreValues = info.signal_ticks.data(),
        };

        const vk::SubmitInfo submit_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = info.num_wait_semas,
            .pWaitSemaphores = info.wait_semas.data(),
            .pWaitDstStageMask = wait_stage_masks.data(),
            .commandBufferCount = static_cast<u32>(cmd_buffers.size()),
            .pCommandBuffers = cmd_buffers.data(),
            .signalSemaphoreCount = info.num_signal_semas,
            .pSignalSemaphores = info.signal_semas.data(),
        };

        ImGui::Core::TextureManager::Submit();
        const auto submit_start = std::chrono::steady_clock::now();
        auto submit_result = queue.submit(submit_info, info.fence);
        Common::GuestStats::queue_submit_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 submit_start)
                .count(),
            std::memory_order_relaxed);
        ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    }
}

void Scheduler::NoteSubmitted(u64 tick) {
    if (submitted_tick.load(std::memory_order_relaxed) < tick) {
        submitted_tick.store(tick, std::memory_order_release);
        submitted_tick.notify_all();
    }
}

void Scheduler::WaitSubmitted(u64 tick) const {
    u64 seen = submitted_tick.load(std::memory_order_acquire);
    while (seen < tick) {
        submitted_tick.wait(seen, std::memory_order_acquire);
        seen = submitted_tick.load(std::memory_order_acquire);
    }
}

vk::CommandBuffer Scheduler::TailCommandBuffer() {
    CatchUp();
    auto& session = sessions.back();
    if (!session.tail) {
        session.tail = tail_pool.Commit();
        Check(session.tail.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    }
    return session.tail;
}

void Scheduler::FlushFrame(SubmitInfo& info) {
    CatchUp();
    // F3, Performance, "Submit on the recording thread" (off: waits for the recording thread
    // as before).
    const bool async = EmulatorSettings.IsBbAsyncSubmit();
    if (!async && recorder) {
        RecordingCosts::sync_submits[0].fetch_add(1, std::memory_order_relaxed);
    }
    if (!async || !TrySubmitOnRecorder(info)) {
        SubmitExecution(info);
    }
}

bool Scheduler::TrySubmitOnRecorder(SubmitInfo& info) {
#if TRACY_GPU_ENABLED
    return false;
#else
    // Only the plain case: one session without an upload command buffer and no capture.
    // (Direct mode at the flip: nobody holds a CommandBuffer() here, as at KickRecording.)
    const auto fall_back = [](size_t why) {
        RecordingCosts::sync_submits[why].fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (!recorder) {
        return false;
    }
    if (sessions.back().upload) {
        return fall_back(1);
    }
    if (sessions.size() != 1) {
        return fall_back(2);
    }
    if (capture || on_session) {
        return fall_back(3);
    }
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        return fall_back(3);
    }
    // The on-submit callback records under submit_mutex, which a submission still queued on the
    // recording thread waits for: the ring must not be able to fill (Kick would wait for it).
    if (!recorder->FarFromFull()) {
        return fall_back(4);
    }
    RecordingCosts::async_submits.fetch_add(1, std::memory_order_relaxed);
    Common::Crumbs::Add("GPU submit on the recording thread (tick)", CurrentTick());
    direct_mode = false;
    u64 signal_value;
    {
        std::scoped_lock lk{submit_mutex};
        signal_value = work_semaphore.NextTick();
        if (on_submit) {
            on_submit(info);
        }
    }
    // As EndSession: the pass ends (the velocity mirror's batch with it) in the queue.
    EndRendering();
    const vk::CommandBuffer tail = sessions.back().tail;
    if (tail) {
        Check(tail.end());
    }
    info.AddSignal(work_semaphore.Handle(), signal_value);
    // GPU timing (the performance meter's GPU load): as SubmitExecution and BeginSession, the
    // timestamps written by the recording thread.
    const vk::QueryPool timer = timer_pool ? *timer_pool : vk::QueryPool{};
    const bool end_stamp = timer && timer_open;
    const u32 end_slot = timer_slot;
    if (end_stamp) {
        if (pending_timings.size() >= TimerPairs - 1) {
            pending_timings.pop_front(); // never read: its slot is about to be reused
        }
        pending_timings.push_back({signal_value, timer_slot});
        timer_slot = (timer_slot + 1) % TimerPairs;
        timer_open = false;
    }
    const bool begin_stamp = timer && gpu_timing;
    const u32 begin_slot = timer_slot;
    if (begin_stamp) {
        timer_open = true;
    }
    sessions.clear();
    sessions.emplace_back();
    primary_pending = true;
    // After the commands queued before it: ends this submission's command buffer, submits it
    // (and the tail), then begins the next one, which the commands queued after it go into.
    recorder->Push([this, info, tail, signal_value, timer, end_stamp, end_slot, begin_stamp,
                    begin_slot](vk::CommandBuffer cmdbuf) {
        if (end_stamp) {
            cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timer,
                                  end_slot * 2 + 1);
        }
        Check(cmdbuf.end());
        {
            // Background texture copies this submission reads must be done.
            const auto start = std::chrono::steady_clock::now();
            if (Common::CopyPool::WaitAsync()) {
                Common::GuestStats::async_copy_wait_ns.fetch_add(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count(),
                    std::memory_order_relaxed);
            }
        }
        std::vector<vk::CommandBuffer> cmd_buffers{cmdbuf};
        if (tail) {
            cmd_buffers.push_back(tail);
        }
        {
            std::scoped_lock lk{submit_mutex};
            QueueOp([queue = instance.GetGraphicsQueue(), info,
                     cmd_buffers = std::move(cmd_buffers)] {
                QueueSubmit(queue, info, cmd_buffers);
            });
        }
        NoteSubmitted(signal_value);
        // The command pool is this thread's while the other one doesn't sync with it.
        const vk::CommandBuffer next = command_pool.Commit();
        Check(next.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
        if (begin_stamp) {
            next.resetQueryPool(timer, begin_slot * 2, 2);
            next.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, timer, begin_slot * 2);
        }
        recorder_primary = next;
        recorder->SetCommandBuffer(next);
    });
    recorder->Kick(true);
    // As BeginSession: the new command buffer starts without dynamic state.
    dynamic_state.Invalidate();
    work_semaphore.Refresh();
    CollectGpuTiming();
    PopPendingOperations();
    return true;
#endif
}

void Scheduler::CollectGpuTiming() {
    while (!pending_timings.empty() && work_semaphore.IsFree(pending_timings.front().tick)) {
        const auto [tick, slot] = pending_timings.front();
        pending_timings.pop_front();
        std::array<u64, 2> stamps{};
        const auto result = instance.GetDevice().getQueryPoolResults(
            *timer_pool, slot * 2, 2, sizeof(stamps), stamps.data(), sizeof(u64),
            vk::QueryResultFlagBits::e64);
        if (result != vk::Result::eSuccess || stamps[1] < stamps[0]) {
            continue;
        }
        const u64 busy = static_cast<u64>((stamps[1] - stamps[0]) * timer_period_ns);
        gpu_busy_ns.fetch_add(busy, std::memory_order_relaxed);
        Common::GuestStats::gpu_timeline_busy_ns.fetch_add(busy, std::memory_order_relaxed);
        Common::GuestStats::gpu_timeline_submits.fetch_add(1, std::memory_order_relaxed);
        if (timer_last_end != 0 && stamps[0] > timer_last_end) {
            const u64 idle = static_cast<u64>((stamps[0] - timer_last_end) * timer_period_ns);
            gpu_idle_ns.fetch_add(idle, std::memory_order_relaxed);
            Common::GuestStats::gpu_timeline_idle_ns.fetch_add(idle, std::memory_order_relaxed);
        }
        timer_last_end = stamps[1];
        gpu_submits.fetch_add(1, std::memory_order_relaxed);
    }
}

std::array<u64, 3> Scheduler::TakeGpuTiming() {
    return {gpu_busy_ns.exchange(0, std::memory_order_relaxed),
            gpu_idle_ns.exchange(0, std::memory_order_relaxed),
            gpu_submits.exchange(0, std::memory_order_relaxed)};
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::MarkCommitted(const Instance& instance) {
    auto& d = dirty_state;
    d.viewports = d.scissors = false;
    d.depth_test_enabled = d.depth_write_enabled = false;
    if (depth_test_enabled) {
        d.depth_compare_op = false;
    }
    d.depth_bounds_test_enabled = false;
    if (depth_bounds_test_enabled) {
        d.depth_bounds = false;
    }
    d.depth_bias_enabled = false;
    if (depth_bias_enabled) {
        d.depth_bias = false;
    }
    d.stencil_test_enabled = false;
    if (stencil_test_enabled) {
        d.stencil_front_ops = d.stencil_back_ops = false;
        d.stencil_front_reference = d.stencil_back_reference = false;
        d.stencil_front_write_mask = d.stencil_back_write_mask = false;
        d.stencil_front_compare_mask = d.stencil_back_compare_mask = false;
    }
    d.primitive_restart_enable = d.rasterizer_discard_enable = false;
    d.cull_mode = d.front_face = false;
    d.blend_constants = d.color_write_masks = d.line_width = false;
    if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        d.feedback_loop_enabled = false;
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
