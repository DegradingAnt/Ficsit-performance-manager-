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
 * ═══ ⚠ NEITHER ROUTE IS VERIFIED HERE, SO THIS FIX DISCOVERS ITS OWN ═══
 *
 * Checked 2026-08-11 and both of the obvious approaches have a hole:
 *   · **No headers.** The SML dev tree's `Plugins/` holds AbstractInstance, FactoryGameUbtPlugin,
 *     GameplayEvents, Online, RainRendering, ReliableMessaging, SignificanceISPC, Wwise, WwiseNiagara —
 *     and no Streamline or DLSS. So `UStreamlineLibraryReflex` cannot be linked against at all.
 *   · **No verified cvar names.** Ant's own client log carries 336 Streamline mentions and **zero**
 *     `t.Streamline.*` strings, so the names in the spec remain exactly as it labelled them: unverified.
 *
 * What IS established, from that same log:
 *     LogPluginManager: Mounting Project plugin StreamlineReflex
 *     LogStreamline: FStreamlineMaxTickRateHandler::Initialize sl::ReflexState::lowLatencyAvailable=1
 * — the plugin is mounted and low-latency is available on her hardware. The capability is real; only
 * the handle on it is uncertain.
 *
 * So this fix tries the cvar, and if the cvar does not exist it goes through the reflection route,
 * and **it says which one it used**. A fix that silently picked a route nobody can name is how a lever
 * ends up "working" for months without anyone able to prove it.
 *
 * ⚠ REFLECTION IS NOT A HACK HERE, IT IS THE ONLY AVAILABLE ROUTE. `SetReflexMode` is a `UFUNCTION`,
 * so the reflection system is its public interface by construction — and unlike a header dependency it
 * degrades to a clear "not found" line on a machine with no Streamline, instead of failing to load the
 * whole module.
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
};
