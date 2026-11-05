// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::BbGameFixes {

// Fixes in Bloodborne 1.09's own code, installed after the patch file at game load. Each checks
// the game's bytes first and is left out when they differ (another version, or a patch there).
void Install();

/// The game's screen-space reflections (Kyo's patch, built in): installed at a start when the
/// reflection setting isn't Off.
enum class SsrPatch {
    Off,           ///< reflections Off at the start: the game's code left as it is
    Installed,     ///< the built-in patch is on
    FromPatchFile, ///< the patch file already had it on
    Unavailable,   ///< the game's code differs (another version, or another patch there)
};
SsrPatch GetSsrPatch();

/// True when this reflection mode (0 the game's, 1 better, 2 off) needs a restart to take effect:
/// reflections switched on with the patch out, or Off with the built-in patch in.
bool SsrNeedsRestart(int ssr_mode);

// True while the game shows a loading screen (respawn, travel, loading a save, quitting to the
// title). Always false when the game's code differs from 1.09's there.
bool IsLoadingScreen();

} // namespace Core::BbGameFixes
