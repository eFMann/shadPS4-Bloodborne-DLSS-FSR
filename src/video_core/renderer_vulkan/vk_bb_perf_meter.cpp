// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_bb_perf_meter.h"

namespace Vulkan::PerfMeter {
namespace {

/// The command thread's steps (Rasterizer::ASection) and the recorder's (Rasterizer::Section).
constexpr std::array<const char*, 6> CmdSteps{"decode", "pipeline", "constants",
                                              "vertex", "index",    "packet"};
constexpr std::array<const char*, 20> RecSteps{
    "packet",     "pending ops", "targets",  "buffers", "tex memo",   "tex lookup",  "tex view",
    "tex layout", "tex writes",  "samplers", "dlss",    "begin pass", "vertex bufs", "index",
    "barriers",   "descriptors", "motion",   "dynamic", "record",     "reset"};
constexpr std::array<const char*, 10> RecDetails{"texture set key",       "texture set checks",
                                                 "texture set binding",   "shader buffer lookups",
                                                 "vertex buffer lookups", "memory sync for shaders",
                                                 "feature observers",     "shader buffer hazards",
                                                 "dynamic state",         "handoff"};

// Command thread states (Common::GuestStats::CmdState).
enum State { Idle, Work, DrainWait, GuestWait, DisplayWait, Command, GpuWait };

/// Loads at which a stage holds the frame rate.
constexpr double GpuFull = 0.90, CmdFull = 0.85, RecorderFull = 0.85, GameStarved = 0.20;

std::mutex mutex;
std::mutex minute_mutex; ///< the summary: the sampler thread's, and Flush's at exit
Reading last;
std::chrono::steady_clock::time_point last_time{};

/// The minute's summary.
struct Summary {
    double seconds{};
    double frames{};
    std::vector<std::pair<std::string, double>> by_limit; ///< seconds by verdict
    double cmd{}, rec{}, gpu{}, game{};                   ///< load-seconds
    double fin{}, fin_seconds{};                          ///< the finish thread's
    double rec_busy_s{};                                  ///< the recorder's busy seconds
    std::array<double, 5> fin_waits{}, fin_wait_s{};
    double gpu_seconds{}; ///< seconds the GPU was measured
    std::array<double, 6> cmd_step_ns{};
    double cmd_timed{};
    std::array<double, 20> rec_step_ns{};
    double rec_timed{};
    std::array<double, 10> rec_detail_ns{};
    double draws{};
};
Summary minute;

void AddLimit(const std::string& name, double seconds) {
    for (auto& [n, s] : minute.by_limit) {
        if (n == name) {
            s += seconds;
            return;
        }
    }
    minute.by_limit.emplace_back(name, seconds);
}

/// The largest steps as "name ms, ..." (per frame when `per` is draws per frame, ns to ms).
template <size_t N>
std::string TopSteps(const std::array<double, N>& ns, double timed, double per,
                     const std::array<const char*, N>& names, size_t count, const char* unit,
                     double scale) {
    if (timed <= 0.0) {
        return {};
    }
    std::array<size_t, N> order{};
    for (size_t i = 0; i < N; ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&](size_t a, size_t b) { return ns[a] > ns[b]; });
    std::string text;
    for (size_t i = 0; i < std::min(count, N); ++i) {
        const double value = ns[order[i]] / timed * per * scale;
        if (value <= 0.0) {
            break;
        }
        text += fmt::format("{}{} {:.{}f}{}", text.empty() ? "" : " ", names[order[i]], value,
                            value < 10.0 ? 1 : 0, unit);
    }
    return text;
}

void LogMinute() {
    if (minute.seconds <= 0.0) {
        return;
    }
    std::ranges::sort(minute.by_limit,
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string limits;
    for (const auto& [name, s] : minute.by_limit) {
        limits += fmt::format("{}{} {:.0f} s", limits.empty() ? "" : ", ", name, s);
    }
    const double s = minute.seconds;
    const double draws_per_frame = minute.frames > 0.0 ? minute.draws / minute.frames : 0.0;
    LOG_INFO(Render_Vulkan,
             "Performance meter: {:.0f} s at {:.1f} fps, {:.0f} draws a frame; seconds by limit: "
             "{}; loads: command thread {:.0f}%, draw recorder {:.0f}%, GPU {}, game starved the "
             "command thread {:.0f}%",
             s, minute.frames / s, draws_per_frame, limits, 100.0 * minute.cmd / s,
             100.0 * minute.rec / s,
             minute.gpu_seconds > 0.0
                 ? fmt::format("{:.0f}%", 100.0 * minute.gpu / minute.gpu_seconds)
                 : std::string{"-"},
             100.0 * minute.game / s);
    if (minute.fin_seconds > 0.0) {
        const double fs = minute.fin_seconds;
        LOG_INFO(Render_Vulkan,
                 "  draw finisher {:.0f}% busy; the recorder waited for it {:.1f}% of the time: "
                 "for commands {:.0f}/s ({:.1f}%), other packets {:.0f}/s ({:.1f}%), our features "
                 "{:.0f}/s ({:.1f}%), ring full {:.0f}/s ({:.1f}%), out of packets {:.0f}/s",
                 100.0 * minute.fin / fs,
                 100.0 *
                     (minute.fin_wait_s[0] + minute.fin_wait_s[1] + minute.fin_wait_s[2] +
                      minute.fin_wait_s[3]) /
                     fs,
                 minute.fin_waits[0] / fs, 100.0 * minute.fin_wait_s[0] / fs,
                 minute.fin_waits[1] / fs, 100.0 * minute.fin_wait_s[1] / fs,
                 minute.fin_waits[2] / fs, 100.0 * minute.fin_wait_s[2] / fs,
                 minute.fin_waits[3] / fs, 100.0 * minute.fin_wait_s[3] / fs,
                 minute.fin_waits[4] / fs);
        if (minute.draws > 0.0) {
            // Busy time a draw on each thread (the recorder's includes its packets other than
            // draws): the one that holds the frame rate is the larger.
            LOG_INFO(Render_Vulkan,
                     "  busy time a draw: draw recorder (resolving) {:.2f} us, draw finisher "
                     "{:.2f} us",
                     minute.rec_busy_s * 1e6 / minute.draws, minute.fin * 1e6 / minute.draws);
        }
    }
    LOG_INFO(Render_Vulkan, "  command thread per draw (us): {}",
             TopSteps(minute.cmd_step_ns, minute.cmd_timed, 1.0, CmdSteps, 6, "", 1e-3));
    LOG_INFO(Render_Vulkan, "  draw recorder per draw (us): {}",
             TopSteps(minute.rec_step_ns, minute.rec_timed, 1.0, RecSteps, 10, "", 1e-3));
    if (minute.rec_timed > 0.0) {
        std::string parts;
        for (size_t i = 0; i < RecDetails.size(); ++i) {
            parts += fmt::format("{}{} {:.2f}", parts.empty() ? "" : ", ", RecDetails[i],
                                 minute.rec_detail_ns[i] / minute.rec_timed * 1e-3);
        }
        LOG_INFO(Render_Vulkan, "  inside those steps (us per draw): {}", parts);
    }
    minute = {};
}

} // namespace

void Update(const Inputs& in) {
    if (in.seconds <= 0.0) {
        return;
    }
    u64 total = 0;
    for (const u64 n : in.states) {
        total += n;
    }
    if (total == 0) {
        return;
    }
    const auto share = [&](std::initializer_list<State> states) {
        u64 n = 0;
        for (const State s : states) {
            n += in.states[s];
        }
        return double(n) / double(total);
    };
    // Loads: the command thread working (decoding, preparing draws, running commands), the
    // recorder's and the GPU's busy time, and the command thread waiting for the game's threads
    // to submit work or write what it waits on.
    const double cmd = share({Work, Command});
    const double game = share({Idle, GuestWait});
    const double rec = std::clamp(in.recorder_busy_s / in.seconds, 0.0, 1.0);
    const double gpu = in.gpu_measured ? std::clamp(in.gpu_busy_s / in.seconds, 0.0, 1.0) : 0.0;
    const double fin = in.finisher_on ? std::clamp(in.finisher_busy_s / in.seconds, 0.0, 1.0) : 0.0;

    std::array<std::pair<const char*, double>, 5> loads{{{"GPU", gpu},
                                                         {"command thread", cmd},
                                                         {"draw recorder", rec},
                                                         {"draw finisher", fin},
                                                         {"game", game}}};
    std::string limit;
    double limit_load = 0.0;
    if (in.stalled) {
        limit = "stall";
    } else if (in.cap_fps > 0.0 && in.fps >= in.cap_fps * 0.97) {
        limit = "capped";
    } else if (gpu >= GpuFull) {
        limit = "GPU";
        limit_load = gpu;
    } else if (cmd >= CmdFull) {
        limit = "command thread";
        limit_load = cmd;
    } else if (rec >= RecorderFull) {
        limit = "draw recorder";
        limit_load = rec;
    } else if (fin >= RecorderFull) {
        limit = "draw finisher";
        limit_load = fin;
    } else if (game >= GameStarved) {
        limit = "game";
        limit_load = game;
    } else {
        limit = "none";
    }
    std::ranges::sort(loads, [](const auto& a, const auto& b) { return a.second > b.second; });
    const auto& next = loads[0].first == limit ? loads[1] : loads[0];

    const double frame_ms = in.fps > 0.0 ? 1000.0 / in.fps : 0.0;
    const double draws_per_frame = in.fps > 0.0 ? in.draws / in.seconds / in.fps : 0.0;
    Reading r;
    r.valid = true;
    r.limit = limit;
    r.limit_load = limit_load;
    r.next = next.first;
    r.next_load = next.second;
    r.cmd_ms = cmd * frame_ms;
    r.rec_ms = rec * frame_ms;
    r.gpu_ms = in.gpu_measured ? gpu * frame_ms : -1.0;
    r.draws_per_frame = draws_per_frame;
    // The command thread's steps per frame in ms: per draw x draws a frame.
    r.cmd_steps = TopSteps(in.cmd_step_ns, in.cmd_timed, draws_per_frame, CmdSteps, 3, "", 1e-6);
    {
        std::scoped_lock lock{mutex};
        last = std::move(r);
        last_time = std::chrono::steady_clock::now();
    }

    std::scoped_lock minute_lock{minute_mutex};
    minute.seconds += in.seconds;
    minute.frames += in.fps * in.seconds;
    AddLimit(limit, in.seconds);
    minute.cmd += cmd * in.seconds;
    minute.rec += rec * in.seconds;
    minute.rec_busy_s += in.recorder_busy_s;
    minute.game += game * in.seconds;
    if (in.finisher_on) {
        minute.fin += fin * in.seconds;
        minute.fin_seconds += in.seconds;
        for (size_t i = 0; i < minute.fin_waits.size(); ++i) {
            minute.fin_waits[i] += in.finisher_waits[i];
            minute.fin_wait_s[i] += in.finisher_wait_s[i];
        }
    }
    if (in.gpu_measured) {
        minute.gpu += gpu * in.seconds;
        minute.gpu_seconds += in.seconds;
    }
    for (size_t i = 0; i < in.cmd_step_ns.size(); ++i) {
        minute.cmd_step_ns[i] += in.cmd_step_ns[i];
    }
    minute.cmd_timed += in.cmd_timed;
    for (size_t i = 0; i < in.rec_step_ns.size(); ++i) {
        minute.rec_step_ns[i] += in.rec_step_ns[i];
    }
    minute.rec_timed += in.rec_timed;
    for (size_t i = 0; i < in.rec_detail_ns.size(); ++i) {
        minute.rec_detail_ns[i] += in.rec_detail_ns[i];
    }
    minute.draws += in.draws;
    if (minute.seconds >= 20.0) {
        LogMinute();
    }
}

void Flush() {
    std::scoped_lock lock{minute_mutex};
    LogMinute();
}

Reading Get() {
    std::scoped_lock lock{mutex};
    if (std::chrono::steady_clock::now() - last_time > std::chrono::seconds(3)) {
        return {};
    }
    return last;
}

} // namespace Vulkan::PerfMeter
