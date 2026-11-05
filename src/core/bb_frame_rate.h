// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 bbport contributors (deadinside28/bloodborne_pc)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/types.h"

namespace Core::BbFrameRate {

// Bloodborne 1.09's frame rate, changed while the game runs: the community's 90 FPS++ patch (by
// Kyo, from the patches folder) written at the start, and its frame rate values (the pacer's for
// the cap, the steps for the frame rate the game holds) rewritten for the cap set in F1. Installed
// at game load when the setting is on; left out when the game or the patch differ.
void Install();

// Before the patch file applies: whether it will be used (the setting, Bloodborne 1.09, the 90
// FPS++ patch in the patches folder).
void Prepare();

// Every write of every enabled patch, before any applies (patch-file address). A frame rate
// patch (one that writes where 90 FPS++ does) is skipped as a whole while it is used.
void CheckPatch(std::string_view name, u64 address, u64 size);
bool ShouldSkipPatch(std::string_view name);

// True when it runs the game's frame rate. The video out then shows a frame as soon as the game
// submits it.
bool Active();

// The cap while the game runs (30..480; 0: none, 480). The game's own frame pacer waits for it.
void SetLimit(int fps);

/// The FPS cap's frame time in seconds, 0 without a cap (or when not active).
float CapPeriod();

/// The frame rate the game holds (smoothed), 0 when not active.
float HeldRate();

} // namespace Core::BbFrameRate
