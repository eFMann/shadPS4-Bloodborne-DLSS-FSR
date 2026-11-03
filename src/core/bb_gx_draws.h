// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"

namespace AmdGpu {
union Regs;
}

namespace Core::BbGxDraws {

// Building draws from the engine's objects, step 1: the capture, checked (F3's Diagnostics, "Check
// GX draw capture"; Bloodborne 1.09).
//
// Bloodborne draws through FromSoftware's GX layer, shaped like Direct3D 11: a draw method of
// GX's context gets the context and its pending state - the shader objects, the input layout,
// the index buffer, the blend, depth-stencil and rasterizer objects, the targets, each stage's
// samplers, views and constant buffers - and writes the GPU commands for them (the registers
// that changed, the draw packet). The emulator's command thread then decodes those commands and
// rebuilds the draw from the register file. Building the draw from the engine's objects instead
// saves that decoding and the lookups behind it (bbhost's approach, github.com/droogie/bbhost,
// docs/rendering.md, whose maps of the 1.09 executable this follows).
//
// This step changes nothing drawn. The six draw methods are hooked where their draw packet has
// just been written: the hook reads what the pending state binds, keyed by where the packet ends
// in the command buffer. When the command thread reaches that packet, it compares the capture
// with what it decoded from the registers and counts agreements and differences by field (the
// log, every 10 s). Everything a later step would draw from has to agree here first.

/// At game load, after the patches (the setting, Bloodborne 1.09's eboot.bin).
void Install();
/// The hooks are in and the command thread checks its draws.
bool Observing();

/// The command thread, at a draw packet: `guest_end` is where the packet ends in the game's
/// command buffer, `tail` the packet's last two dwords as the command thread read them, `regs`
/// the register file it draws with.
void CheckDraw(u64 guest_end, std::span<const u32, 2> tail, const AmdGpu::Regs& regs);

/// The command thread, before working a draw's pipeline out: the pipeline the draw CheckDraw saw
/// took when its objects were seen before ("Pipelines from the engine's objects"), else null.
/// `generation` is the pipeline cache's (remembered pipelines are forgotten when it changes).
const void* RememberedPipeline(u64 generation);

/// The command thread, once the draw CheckDraw saw has its pipeline (from the registers): whether
/// the engine's objects would have told the same one (the pipeline memo check).
void NotePipeline(const void* pipeline);

} // namespace Core::BbGxDraws
