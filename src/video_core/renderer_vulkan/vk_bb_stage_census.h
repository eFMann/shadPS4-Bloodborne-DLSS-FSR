// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Recorder split census (F3, Diagnostics): the map for splitting the draw recorder in two by
// ownership, as the threaded renderer split the GPU command thread from the recorder.
//
// The plan: a resolver that owns the caches (textures, buffers, image layouts, uploads) and a
// finisher that owns the commands (render passes, barriers emitted, descriptors, the draw),
// each draw handed from one to the other in a packet. For that every step of a draw has to
// keep to its side. The census marks the recorder's draw in two phases where the split would
// go - resolve (render targets, textures, buffers, vertex and index buffers) and finish
// (barriers, descriptors, dynamic state, the render pass, the draw, the hooks after it) - and
// counts the calls into the caches and into command recording made in each. A call into the
// caches while finishing, or a command recorded while resolving, crosses the line: those are
// what the split has to move or hand over. Counted by the call's place; logged every 20 s.

#pragma once

#include <array>
#include <atomic>

#include "common/types.h"

namespace Vulkan::StageCensus {

enum class Phase : u8 { None, Resolve, Finish };
/// Whose state a counted call belongs to after the split.
enum class Owner : u8 {
    Caches,    ///< the resolver's: texture and buffer caches, image layouts, buffer tracking
    Commands,  ///< the finisher's: recorded commands, render passes
    Features,  ///< ours (upscaler, VBAO, reflections, occlusion, mirror): see where they run
    Registers, ///< the draw's GPU registers: the resolve half moves on to the next draw's
};

/// Where in the draw a call happened (the recorder's own steps), for the crossings.
enum class Label : u8 {
    Targets,     ///< render targets found (PrepareRenderState)
    Resources,   ///< textures and shader buffers bound
    Upscaler,    ///< the upscaler's draw hook
    RenderState, ///< the render pass's attachments worked out
    Vertex,      ///< vertex buffers
    Index,       ///< index buffer
    Barriers,    ///< barriers emitted
    Motion,      ///< character motion
    Descriptors, ///< descriptor writes
    Dynamic,     ///< dynamic state
    Pass,        ///< the pass marked (MarkPass) and begun
    Draw,        ///< pipeline bound, the draw, occlusion queries
    Mirror,      ///< velocity mirror replay
    DisplayCopy, ///< display copy replay
    Hooks,       ///< VBAO and reflections after the draw
    Reset,       ///< bindings reset
    Count,
};
inline constexpr std::array<const char*, static_cast<size_t>(Label::Count)> LabelNames{
    "targets",      "resources", "upscaler",        "render state", "vertex buffers",
    "index buffer", "barriers",  "motion",          "descriptors",  "dynamic state",
    "pass",         "draw",      "velocity mirror", "display copy", "VBAO/SSR after the draw",
    "reset"};

inline std::atomic<bool> enabled{false};
inline thread_local Phase phase = Phase::None;
inline thread_local Label label = Label::Targets;

struct Site {
    const char* name;
    Owner owner;
    std::array<std::atomic<u64>, 2> calls{}; ///< resolve, finish
    std::array<std::atomic<u64>, static_cast<size_t>(Label::Count)> by_label{};
    std::atomic<bool> listed{false};
};
void Count(Site& site);

/// The recorder, around a draw's two phases.
class PhaseScope {
public:
    explicit PhaseScope(Phase next) : previous{phase} {
        phase = next;
    }
    ~PhaseScope() {
        phase = previous;
    }
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;

private:
    Phase previous;
};

/// The recorder, once a draw is recorded: counts it and logs every 20 s.
void DrawDone();

} // namespace Vulkan::StageCensus

/// At the start of a function the split has to place: counted while the census is on and the
/// recorder is in one of a draw's phases.
#define BB_STAGE_CENSUS(name, owner)                                                               \
    do {                                                                                           \
        if (::Vulkan::StageCensus::enabled.load(std::memory_order_relaxed) &&                      \
            ::Vulkan::StageCensus::phase != ::Vulkan::StageCensus::Phase::None) [[unlikely]] {     \
            static ::Vulkan::StageCensus::Site census_site_{name,                                  \
                                                            ::Vulkan::StageCensus::Owner::owner};  \
            ::Vulkan::StageCensus::Count(census_site_);                                            \
        }                                                                                          \
    } while (false)
