// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * THE MASTER ON/OFF SWITCH — and OFF has to mean RELEASED, not merely "stopped writing".
 *
 * P4.2 (`_DESIGN-R2-2026-08-09.md` §PHASE 4). Ships as a console variable rather than a settings row,
 * on purpose: `UFGUserSetting` assets are editor work and do not exist yet, and building the whole
 * consumption layer before any row exists is unwired infrastructure — the old mod's single most
 * repeated fault. The row, when it arrives, drives this cvar. The cvar has a consumer today.
 *
 * ★ WHY "OFF" IS THE HARD HALF.
 *
 * Stopping FPM from writing more console variables is trivial and WRONG. Every value FPM has already
 * pushed stays applied, so a user who turns the mod off still has FPM's settings on their machine — and
 * `.uplugin` says, in the Description a player reads on ficsit.app, that it *"leaves nothing behind"*.
 * So OFF routes through `FPMCVarWriter::ReleaseAll`, which restores each cvar's prior value AND prior
 * SetBy priority, and which already proves that path on its own probe cvar at every boot.
 *
 * ⚠ AND OFF WAS A LIE UNTIL 0.11.9. `FPMFixes::DisarmAll()` walks the armed list calling `Disarm()`,
 * but `IFPMFix::Disarm()` defaults to `{}` and 13 fixes that installed hooks never overrode it. This
 * switch could not have shipped before that was repaired: it would have reported FPM disabled while
 * seventeen hooks stayed installed, and the fix inventory would have read zero armed — the inventory
 * lying being the one thing `FPMHookLedger` exists to prevent.
 *
 * ★ IT IS REVERSIBLE, and that needed a registry change. `DisarmAll` empties the armed list, and the
 * arm sequence lives in `StartupModule`, so without `FPMFixes::RearmAll()` an OFF would have been
 * permanent for the session.
 *
 * ⚠ WHAT ON DOES NOT RESTORE, stated because a half-true report is worse than none: a fix whose real
 * work happens in `OnWorldLoad` comes back ARMED BUT INERT until the next world load. Re-arming does
 * not replay one. Both the log line and `FPM.Enabled`'s help text say so.
 */
namespace FPMMasterSwitch
{
	/** Registers the cvar sink. Call once from `StartupModule`, AFTER every fix has armed. */
	FICSITSPERFORMANCEMANAGER_API void Install();

	/** True when FPM is currently enabled. Reads the live cvar, not a cached copy. */
	FICSITSPERFORMANCEMANAGER_API bool IsEnabled();
}
