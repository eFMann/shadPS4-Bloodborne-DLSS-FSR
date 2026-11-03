// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/arch.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/types.h"

namespace Common {

#ifdef _MSC_VER
__forceinline static u64 FencedRDTSC() {
#ifdef ARCH_X86_64
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 result = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return result;
#else
#error "Missing FencedRDTSC() implementation for target CPU architecture."
#endif
}
#else
static inline u64 FencedRDTSC() {
#ifdef ARCH_X86_64
    u64 eax;
    u64 edx;
    asm volatile("lfence\n\t"
                 "rdtsc\n\t"
                 "lfence\n\t"
                 : "=a"(eax), "=d"(edx));
    return (edx << 32) | eax;
#elif defined(ARCH_ARM64)
    u64 ret;
    asm volatile("isb\n\t"
                 "mrs %0, cntvct_el0\n\t"
                 "isb\n\t"
                 : "=r"(ret)::"memory");
    return ret;
#else
#error "Missing FencedRDTSC() implementation for target CPU architecture."
#endif
}
#endif

u64 EstimateRDTSCFrequency();

/// Nanoseconds a TSC tick, measured from the first call to now (no waiting; rough during the
/// first few milliseconds). For converting busy times counted in ticks: reading the TSC costs a
/// fraction of the system clock (QueryPerformanceCounter was ~2% of all CPU time at many reads
/// a draw).
double TscNsPerTick();

} // namespace Common
