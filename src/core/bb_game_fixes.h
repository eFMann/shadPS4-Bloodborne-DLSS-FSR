// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::BbGameFixes {

// Fixes in Bloodborne 1.09's own code, installed after the patch file at game load. Each checks
// the game's bytes first and is left out when they differ (another version, or a patch there).
void Install();

/// The game's screen-space reflections (Kyo's patch, built in): installed at every start, the
/// reflection setting (the game's, better or off) then applies at once.
enum class SsrPatch {
    Installed,     ///< the built-in patch is on
    FromPatchFile, ///< the patch file already had it on
    Unavailable,   ///< the game's code differs (another version, or another patch there)
};
SsrPatch GetSsrPatch();

// True while the game shows a loading screen (respawn, travel, loading a save, quitting to the
// title). Always false when the game's code differs from 1.09's there.
bool IsLoadingScreen();

} // namespace Core::BbGameFixes
