// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Performance meter (Bloodborne, threaded renderer; F3's Diagnostics): once a second, which stage
// holds the frame rate, after bbhost's bottleneck meter (github.com/droogie/bbhost,
// docs/performance.md). A frame passes through the game's own threads (they record and submit
// command buffers), the GPU command thread (decodes them, selects pipelines, prepares draws),
// the draw recorder (records the Vulkan commands) and the GPU; the slowest is the frame rate's.
//
// The verdict comes from what the stages measure anyway: the command thread's state sampled
// every millisecond (working, waiting for the game, for the recorder, for the GPU), the draw
// recorder's busy time, the GPU's busy time per submission (timestamps), and every 8th draw's
// time on the command thread and the recorder by step. It shows under the FPS counter and goes
// to the log every 20 seconds and when the game closes.

#pragma once

#include <array>
#include <string>

#include "common/types.h"

namespace Vulkan::PerfMeter {

/// What the meter's second gathered (the rasterizer's sampler thread fills it).
struct Inputs {
    double seconds{};
    /// Samples of the command thread's state (Common::GuestStats::CmdState order).
    std::array<u64, 7> states{};
    double recorder_busy_s{}; ///< the draw recorder's busy time (0 without the threaded renderer)
    double gpu_busy_s{};      ///< the GPU's busy time over the submissions it measured
    bool gpu_measured{};      ///< the GPU's timestamps came in
    double fps{};             ///< game frames per second
    double draws{};           ///< draws and dispatches handed to the draw recorder
    /// Every 8th draw's time by step on the command thread and on the recorder (summed ns) and
    /// how many draws were timed.
    std::array<double, 6> cmd_step_ns{};
    double cmd_timed{};
    std::array<double, 20> rec_step_ns{};
    double rec_timed{};
    /// Parts of the recorder's steps on the same draws (Rasterizer::RecDetail).
    std::array<double, 10> rec_detail_ns{};
    /// The draw recorder's finish thread (recorder on two threads): on, its busy time, and how
    /// often and how long the recorder waited for it, by reason (Rasterizer::FinishWait order).
    bool finisher_on{};
    double finisher_busy_s{};
    std::array<double, 5> finisher_waits{};
    std::array<double, 5> finisher_wait_s{};
    double cap_fps{}; ///< the frame rate cap held by the frame rate hooks (0: none)
    bool stalled{};   ///< long frames took a fifth of the second (loading, pipeline builds)
};

/// The last second's verdict, for the counter.
struct Reading {
    bool valid{};
    std::string limit; ///< "GPU", "command thread", "draw recorder", "game", "capped", ...
    double limit_load{};
    std::string next;
    double next_load{};
    double cmd_ms{}, rec_ms{}, gpu_ms{}; ///< busy time per frame
    double draws_per_frame{};
    std::string cmd_steps; ///< the command thread's largest steps per frame, "decode 3.1 ..."
};

/// From the sampler thread, once a second: works out the verdict, publishes it and adds it to
/// the minute's summary.
void Update(const Inputs& in);
/// The summary of what the meter gathered since the last one (it writes one every 20 s), at once:
/// when the game closes.
void Flush();
/// The last verdict (invalid when the meter has not run for a while).
Reading Get();

} // namespace Vulkan::PerfMeter
