// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * DIAGNOSTICS YOU CAN TURN ON AND OFF FROM THE CONSOLE, WITHOUT A REBUILD.
 *
 * Ant, 2026-08-08: *"we should add more diagnostic we can turn on and off at will"* and *"diagnostics
 * are good either way so we KNOW what breaks and why"*.
 *
 * WHY THIS EXISTS RATHER THAN MORE ad-hoc CVARS. The rain fix already ships `FPM.Rain.Sweep` and
 * `FPM.Rain.Hooks`, hand-rolled in its own translation unit. That worked, and repeating it per fix gives
 * six different naming schemes, six different defaults, and no way to silence everything at once when a
 * log needs reading. One facility, one naming rule, one master switch.
 *
 * ⚠ THE COST OF A DISABLED DIAGNOSTIC MUST BE ~ZERO, because the last attempt at schematic diagnostics
 * froze the game — 0.58.54 logged and flushed on every call while the HUB enumerates every schematic in
 * existence. `IsOn()` is a cached int compare, and the correct shape at a call site is to test it BEFORE
 * building any string:
 *
 *     if (FPMDiag::IsOn(FPMDiag::EChannel::SchematicProbe))   // int compare, no allocation
 *     {
 *         UE_LOG(..., TEXT("%s"), *Expensive->GetName());     // only now
 *     }
 *
 * ⚠ ZERO RESIDUE IS NOT THREATENED. These are OUR cvar names, registered by this module and gone when it
 * unloads. The standing law bans WRITING a vanilla `US_*`-backed cvar, and bans `ECVF_SetByConsole` from
 * mod code; declaring our own console variables is neither. Nothing here is written to any ini.
 *
 * LEVELS
 *   0  off      — silent. The hook still runs and still COUNTS; only the printing stops.
 *   1  on       — the default. Throttled Display/Warning lines, plus every anomaly, unthrottled.
 *   2  verbose  — per-call detail. Expect volume; this is for one deliberate boot, not for playing.
 *
 * ⚠ ONE STATED EXCEPTION TO "0 = SILENT": THE ARMED LINE.
 * Each fix prints one line at Arm() time regardless of level. It is emitted from StartupModule, before
 * any console command could have been typed, so gating it would silence it always rather than on
 * request. And it is the line that distinguishes "armed and saw nothing" from "never armed" — the
 * exact ambiguity that let the old mod's milestone override survive three narrowings without anyone
 * being able to prove it installed. **An unstated exception is a broken contract; this one is stated.**
 * Nothing else escapes the switch: found by review 2026-08-08, which caught SEVEN ungated log sites
 * across three fixes after this header already promised they were silenced.
 *
 * A DISABLED CHANNEL NEVER DISABLES A FIX. Turning diagnostics off must never change what the mod DOES,
 * only what it says — otherwise a quiet log becomes evidence of nothing rather than evidence of calm.
 * The rain kill-switches are a different thing and keep their own names: they change BEHAVIOUR.
 */
class FICSITSPERFORMANCEMANAGER_API FPMDiag
{
public:
	/** One channel per diagnostic surface. Keep in sync with the cvar table in the .cpp. */
	enum class EChannel : uint8
	{
		// ★ EVERY FIX MUST NAME ONE — `IFPMFix::Channel()` is a pure virtual as of P1.1, so a fix without
		// a channel does not compile. StaticBase / RpcGate / Rain were added here for exactly that reason:
		// they had diagnostics but no channel, which is the drift the pure virtual now prevents.
		StaticBase,       // FPM.Diag.StaticBase
		RpcGate,          // FPM.Diag.RpcGate
		Rain,             // FPM.Diag.Rain — the SWEEP's reporting, not FPM.Rain.* which change BEHAVIOUR
		SchematicProbe,   // FPM.Diag.Schematic
		HologramNet,      // FPM.Diag.Hologram
		InventoryInit,    // FPM.Diag.Inventory
		CloneSensor,      // FPM.Diag.Clone
		Hitch,            // FPM.Diag.Hitch    — frame-time meter
		Residency,        // FPM.Diag.Residency — the vanilla-asset pin
		Overlay,          // FPM.Diag.Overlay  — the on-screen feed
		SaveGuard,        // FPM.Diag.SaveGuard — the SaveSettings interceptor
		RailGuard,        // FPM.Diag.RailGuard  — unwired rail-connection guard
		NavMesh,          // FPM.Diag.NavMesh   — the tile-ceiling write-back
		HudGuard,         // FPM.Diag.HudGuard  — the blueprint-hook descriptor strip
		Zipline,          // FPM.Diag.Zipline   — the zipline output-bus volume
		TexturePool,      // FPM.Diag.TexturePool — the card-sized streaming pool
		WireGuard,        // FPM.Diag.WireGuard — null mWires entries, found before the save walks them
		WwiseGate,        // FPM.Diag.WwiseGate — StopActor no-ops suppressed on a dedicated server
		Count
	};

	/** True when this channel is at or above `Level`. Cheap enough for a hot path: one int compare. */
	static bool IsOn(EChannel Channel, int32 Level = 1);

	/** The channel's current level, for a fix that wants to branch on verbose vs normal. */
	static int32 LevelOf(EChannel Channel);

	/**
	 * True when the MASTER switch is explicitly 0 — "silence everything", regardless of channel.
	 *
	 * This exists for the one code path that legitimately cannot know its channel: `FPMOverlay::Post`
	 * is shared by every fix and takes only a category string. Its always-log behaviour is correct and
	 * deliberate — a line on screen but absent from `FactoryGame.log` is one nobody can send us
	 * afterwards — so it is NOT gated per channel. But it must still honour `FPM.Diag 0`, because a
	 * master switch that something ignores is not a master switch.
	 *
	 * Per-channel gating belongs at the CALL SITE, where the channel is known. This is the floor.
	 */
	static bool IsSilenced();

	/** The channel's cvar name, e.g. "FPM.Diag.Rain". Used by the fix inventory so a reader can go
	 *  straight from a fix to the switch that silences it. */
	static const TCHAR* NameOf(EChannel Channel);

	/** Prints every channel and its level. Bound to `FPM.Diag.List`. */
	static void LogAll();
};
