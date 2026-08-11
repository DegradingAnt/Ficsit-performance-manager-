// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ NVIDIA REFLEX — carried back from FPM1, which shipped it and FPM2 dropped.
 *
 * Ant, 2026-08-11: *"check the old mod for what we did there since we forced reflex i believe"*, then
 * *"we need that back in fpm2"*. FPM1 had `Gfx_ReflexMode` — *"NVIDIA Reflex (0/1/2)"* —
 * (`FPMModConfiguration.cpp:368`). This is that, rebuilt.
 *
 * ═══ WHAT THE BANKED SPEC SAYS, AND IT IS UNUSUALLY SPECIFIC ═══
 *
 * `FPG-REBUILD-SPEC.md:76`:
 *   · mode 1 (`eLowLatency`) helps when GPU-bound, costs **≤4% FPS**, near-free otherwise;
 *   · mode 2 (Boost) **can cost FPS and power** — gate it, *"never Boost as baseline"*;
 *   · *"Drive via `UStreamlineLibraryReflex::SetReflexMode` — literal `t.Streamline.Reflex.*` names are
 *     UNVERIFIED"*;
 *   · mind `t.Streamline.Reflex.HandleMaxTickRate` — a fight with a frame limiter shows up as judder.
 *
 * ═══ ★★★ THE CVAR NAMES ARE VERIFIED. THE SPEC SAID THEY WERE NOT. ═══
 *
 * Ant, 2026-08-11: *"check the dumps for how to do it"* — and that settled it in one command. UTF-16
 * extraction from `FactoryGame/Plugins/StreamlineCore/Binaries/Win64/
 * FactoryGameSteam-StreamlineCore-Win64-Shipping.dll` returns the names WITH their help text:
 *
 *     "Enable Streamline Reflex extension. (default = 0)"     t.Streamline.Reflex.Enable
 *     "Streamline Reflex mode (default = 1)
 *        1: low latency
 *        2: low latency with boost"                           t.Streamline.Reflex.Mode
 *     "Enable Streamline Reflex extension when other SL
 *      features need it. (default = 1)"                       t.Streamline.Reflex.Auto
 *     "Controls whether Streamline Reflex handles frame rate
 *      limiting instead of the engine (default = true)"       t.Streamline.Reflex.HandleMaxTickRate
 *
 * Same technique that produced the DLSS preset letter-map. `FPG-REBUILD-SPEC.md:76` should be updated:
 * its *"literal `t.Streamline.Reflex.*` names are UNVERIFIED"* is now false, and its mode numbering is
 * confirmed correct.
 *
 * ⚠⚠ AND THE EXTRACTION CAUGHT A DEFECT IN THIS FILE'S FIRST VERSION.
 * **`Reflex.Enable` ships at 0 — Reflex is OFF in this game.** The first version set only `.Mode`,
 * whose default is already 1, so it would have written a mode onto a DISABLED extension and reported
 * success forever. That is the dead-instrument shape, in the fix whose own header warns about it, and
 * only the help text revealed it. **Setting a mode is not enabling a feature.**
 *
 * ═══ WHY THE REFLECTION FALLBACK STAYS ANYWAY ═══
 *
 * The headers genuinely are absent — the SML dev tree's `Plugins/` holds AbstractInstance,
 * FactoryGameUbtPlugin, GameplayEvents, Online, RainRendering, ReliableMessaging, SignificanceISPC,
 * Wwise and WwiseNiagara, and no Streamline — so `UStreamlineLibraryReflex` still cannot be linked
 * against. The cvars are the primary route now that they are proven; the reflection path remains for
 * the case where a game update renames them, and the fix NAMES whichever route it used so a silent
 * switch between them is impossible.
 *
 * Corroboration that the capability is real, from Ant's own client log:
 *     LogPluginManager: Mounting Project plugin StreamlineReflex
 *     LogStreamline: FStreamlineMaxTickRateHandler::Initialize sl::ReflexState::lowLatencyAvailable=1
 */
class FICSITSPERFORMANCEMANAGER_API FFPMReflexMode final : public IFPMFix
{
public:
	static FFPMReflexMode& Get();

	virtual const TCHAR* Name() const override { return TEXT("reflex-mode"); }

	/** Latency pacing is a client concern. A dedicated server renders nothing and has no Streamline. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * OriginNamed. This is not repairing a bug — it is turning on a vendor feature the game leaves off,
	 * with a measured cost from the banked spec rather than a hoped-for one.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Reflex; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** Applies the configured mode once a world exists — Streamline is not up at module startup. */
	virtual void OnWorldLoad(UWorld* World) override;

	/**
	 * ⚠ OFF UNTIL ONE BOOT PROVES THE ROUTE, and that is about THIS CODE, not about Reflex.
	 *
	 * The spec's recommendation is unambiguous — *"KEEP mode 1 (live toggle, default-on)"* — and it
	 * should become the default. But neither mechanism this fix can use has been verified to exist on
	 * her machine, so arming it by default would mean shipping a lever that might silently reach
	 * nothing. That is precisely the shape that cost a whole session on 2026-08-11, when three fixes
	 * reported themselves armed and healthy while doing nothing or doing harm.
	 *
	 * `FPM.Fix.ReflexMode 1` arms it; `FPM.Reflex.Report` then says which route it found. When a boot
	 * shows a route working, this flips to `true` and the default mode stays 1.
	 */
	virtual bool DefaultArmed() const override { return false; }

	/** `FPM.Reflex.Report` — which route was found, what mode is set, and what the plugin reports. */
	static void ReportNow();

private:
	/** Applies FPM.Reflex.Mode through whichever route exists. Logs the route ONCE, then only changes. */
	void ApplyFromCVar(const TCHAR* Moment);

	/** Which mechanism was found, for the report. Empty until Arm() has looked. */
	FString RouteFound;

	int32 AppliedMode = -1;

	/**
	 * What `t.Streamline.Reflex.Enable` / `.Mode` held BEFORE our first write, so Disarm restores what
	 * was actually there rather than what the DLL documents as the default. -1 = not captured yet.
	 */
	int32 PriorEnable = -1;
	int32 PriorMode = -1;
};
