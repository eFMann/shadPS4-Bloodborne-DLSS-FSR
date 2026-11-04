// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Keep render threads off core 0 (F3, Performance; Windows). Windows serves most hardware
// interrupts and much of its own work on the first physical core, which was about twice as busy
// as the others in profiles; the emulator's critical render threads (command thread, draw
// recorder and finisher, recording thread) landing there were interrupted more often. With the
// option on they may run on every logical CPU except those of that core; the game's threads,
// the copy threads and everything else keep all of them.

#pragma once

namespace Common::RenderAffinity {

/// Turns the rule on or off (at startup and when the setting changes). Threads pick it up at
/// their next Apply().
void Set(bool off_core0);

/// Called by each render thread in its loop: applies the rule to the calling thread when it
/// changed since the thread last looked (one relaxed load otherwise).
void Apply();

} // namespace Common::RenderAffinity
