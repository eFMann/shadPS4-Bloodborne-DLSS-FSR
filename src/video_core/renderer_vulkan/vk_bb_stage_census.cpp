// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_bb_stage_census.h"

namespace Vulkan::StageCensus {
namespace {

std::mutex sites_mutex;
std::vector<Site*> sites;
std::atomic<u64> draws{0};
std::chrono::steady_clock::time_point window_start{};

const char* OwnerName(Owner owner) {
    switch (owner) {
    case Owner::Caches:
        return "caches";
    case Owner::Commands:
        return "commands";
    case Owner::Registers:
        return "registers";
    default:
        return "ours";
    }
}

void Report() {
    const u64 n = draws.exchange(0, std::memory_order_relaxed);
    if (n == 0) {
        return;
    }
    struct Line {
        Site* site;
        u64 resolve, finish;
        std::array<u64, static_cast<size_t>(Label::Count)> by_label{};
    };
    std::vector<Line> lines;
    {
        std::scoped_lock lock{sites_mutex};
        // One line a name: a template (Scheduler::Record) counts at each of its instances.
        for (Site* site : sites) {
            const u64 resolve = site->calls[0].exchange(0, std::memory_order_relaxed);
            const u64 finish = site->calls[1].exchange(0, std::memory_order_relaxed);
            std::array<u64, static_cast<size_t>(Label::Count)> by_label{};
            for (size_t i = 0; i < by_label.size(); ++i) {
                by_label[i] = site->by_label[i].exchange(0, std::memory_order_relaxed);
            }
            const auto same = std::ranges::find_if(
                lines, [&](const Line& l) { return std::string_view{l.site->name} == site->name; });
            if (same != lines.end()) {
                same->resolve += resolve;
                same->finish += finish;
                for (size_t i = 0; i < by_label.size(); ++i) {
                    same->by_label[i] += by_label[i];
                }
            } else {
                lines.push_back({site, resolve, finish, by_label});
            }
        }
    }
    const auto crosses = [](const Line& l) {
        return ((l.site->owner == Owner::Caches || l.site->owner == Owner::Registers) &&
                l.finish != 0) ||
               (l.site->owner == Owner::Commands && l.resolve != 0);
    };
    std::ranges::sort(lines, [&](const Line& a, const Line& b) {
        if (crosses(a) != crosses(b)) {
            return crosses(a);
        }
        return a.resolve + a.finish > b.resolve + b.finish;
    });
    LOG_INFO(Render_Vulkan,
             "Recorder split census: {} draws; calls a draw while resolving / finishing (a call "
             "into the caches while finishing, or a command recorded while resolving, crosses)",
             n);
    for (const auto& l : lines) {
        if (l.resolve + l.finish == 0) {
            continue;
        }
        // Where in the draw: the steps with a call, most first.
        std::string where;
        std::array<size_t, static_cast<size_t>(Label::Count)> order{};
        for (size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, [&](size_t a, size_t b) { return l.by_label[a] > l.by_label[b]; });
        for (const size_t i : order) {
            if (l.by_label[i] == 0 || l.by_label[i] * 1000 < l.resolve + l.finish) {
                continue; // under 0.1% of its calls
            }
            where += fmt::format("{}{} {:.3f}", where.empty() ? "" : ", ", LabelNames[i],
                                 double(l.by_label[i]) / double(n));
        }
        LOG_INFO(Render_Vulkan, "  {}{} ({}): {:.3f} / {:.3f} [{}]", crosses(l) ? "CROSSES " : "",
                 l.site->name, OwnerName(l.site->owner), double(l.resolve) / double(n),
                 double(l.finish) / double(n), where);
    }
}

} // namespace

void Count(Site& site) {
    if (!site.listed.exchange(true, std::memory_order_relaxed)) {
        std::scoped_lock lock{sites_mutex};
        sites.push_back(&site);
    }
    site.calls[phase == Phase::Resolve ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
    site.by_label[static_cast<size_t>(label)].fetch_add(1, std::memory_order_relaxed);
}

void DrawDone() {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    draws.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (window_start == std::chrono::steady_clock::time_point{}) {
        window_start = now;
    } else if (now - window_start >= std::chrono::seconds(20)) {
        Report();
        window_start = now;
    }
}

} // namespace Vulkan::StageCensus
