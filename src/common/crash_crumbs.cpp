// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

#include "common/crash_crumbs.h"
#include "common/rdtsc.h"
#include "common/logging/log.h"
#include "common/thread.h"

namespace Common::Crumbs {
namespace {

constexpr u32 RingSize = 64;   // steps kept per thread
constexpr u32 MaxThreads = 48; // threads that leave crumbs (the rest are not tracked)
constexpr u32 Printed = 24;    // steps printed per thread

struct Entry {
    std::atomic<u64> time_us{};
    std::atomic<const char*> what{};
    std::atomic<u64> a{};
    std::atomic<u64> b{};
};

struct ThreadRing {
    std::atomic<bool> used{};
    char name[32]{};
    std::atomic<u32> next{};
    std::array<Entry, RingSize> entries{};
};

std::array<ThreadRing, MaxThreads> g_rings;
std::atomic<u32> g_claimed{0};
std::atomic<bool> g_dumped{false};
/// Steps are stamped with the TSC (a few ns; the system clock cost ~25 ns a step, several steps a
/// draw), converted to time when printed.
u64 NowTicks() {
    return Common::FencedRDTSC();
}

ThreadRing* CurrentRing() {
    thread_local ThreadRing* ring = []() -> ThreadRing* {
        const u32 index = g_claimed.fetch_add(1, std::memory_order_relaxed);
        if (index >= MaxThreads) {
            return nullptr;
        }
        ThreadRing& r = g_rings[index];
        const std::string name = GetCurrentThreadName();
        std::strncpy(r.name, name.empty() ? "unnamed" : name.c_str(), sizeof(r.name) - 1);
        r.used.store(true, std::memory_order_release);
        return &r;
    }();
    return ring;
}

} // namespace

void Add(const char* what, u64 a, u64 b) noexcept {
    ThreadRing* ring = CurrentRing();
    if (!ring) {
        return;
    }
    const u32 n = ring->next.load(std::memory_order_relaxed);
    Entry& e = ring->entries[n % RingSize];
    e.what.store(nullptr, std::memory_order_relaxed); // half-written while printed: skipped
    e.time_us.store(NowTicks(), std::memory_order_relaxed);
    e.a.store(a, std::memory_order_relaxed);
    e.b.store(b, std::memory_order_relaxed);
    e.what.store(what, std::memory_order_release);
    ring->next.store(n + 1, std::memory_order_release);
}

void Dump() noexcept {
    if (g_dumped.exchange(true)) {
        return;
    }
    const u64 now = NowTicks();
    const double ms_per_tick = Common::TscNsPerTick() / 1e6;
    const u32 threads = std::min(g_claimed.load(std::memory_order_acquire), MaxThreads);
    LOG_CRITICAL(Debug,
                 "Crash breadcrumbs: the last steps of {} threads (ms before the crash):", threads);
    for (u32 t = 0; t < threads; ++t) {
        const ThreadRing& ring = g_rings[t];
        if (!ring.used.load(std::memory_order_acquire)) {
            continue;
        }
        const u32 next = ring.next.load(std::memory_order_acquire);
        if (next == 0) {
            continue;
        }
        const u32 count = std::min(next, Printed);
        LOG_CRITICAL(Debug, "  thread {} ({} steps in all):", ring.name, next);
        for (u32 i = next - count; i < next; ++i) {
            const Entry& e = ring.entries[i % RingSize];
            const char* what = e.what.load(std::memory_order_acquire);
            if (!what) {
                continue;
            }
            const u64 at = e.time_us.load(std::memory_order_relaxed);
            LOG_CRITICAL(Debug, "    -{:.3f} {} {:#x} {:#x}",
                         double(now > at ? now - at : 0) * ms_per_tick, what,
                         e.a.load(std::memory_order_relaxed), e.b.load(std::memory_order_relaxed));
        }
    }
}

} // namespace Common::Crumbs
