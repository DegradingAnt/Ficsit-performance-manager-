// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * GARBAGE-COLLECTION METER — how often the world stops, for how long, and whether anything can be done.
 *
 * Board `m508019305` / `m6253024`. Ant's report is *"hitches moving fast through the world"*, and the
 * asset-residency fix already took the async-load half. GC is the other, separate class.
 *
 * ★ IT IS L1 OF A DESIGN THAT WAS ALREADY WRITTEN AND THEN ORPHANED. `fpm-design-gc-stutter.md`
 * (2026-08-02) closed most of the surface with receipts before proposing anything, and the rewrite lost
 * it. Its measured baseline, from a primary log on Ant's save with 17,847 buildables: **27 GC pauses in
 * ~22 minutes, mean 27.2 ms, worst 148.6 ms**, every one stop-the-world.
 *
 * ★ WHY MEASUREMENT SHIPS BEFORE ANY LEVER, and this is the design's own ordering. The one real pacing
 * lever — raising `gc.TimeBetweenPurgingPendingKillObjects` — only halves the TIMER-driven passes.
 * Forced passes are untouched. The design's own arithmetic says 22 min / 60 s = 22 timer passes against
 * 27 observed, so roughly **five were forced** — but it labels that a HYPOTHESIS, because the forcing
 * call sites are invisible from here (the SML stubs have empty bodies). If most passes turn out to be
 * forced, the pacing lever's yield collapses and the follow-up should target the forcer instead. One
 * boot of this meter decides which, at zero behavioural risk.
 *
 * ★ WHAT IS ALREADY CLOSED, so nobody re-proposes it:
 *  - **Incremental reachability is a banked dead end.** `gc.AllowIncrementalReachability 1` crashed the
 *    game in 34 seconds on Ant's save. FPM2 ships no switch for it — verified by grep, zero hits — and
 *    that is one case where the rewrite dropping something was exactly right.
 *  - **Parallel mark is already on** (`GAllowParallelGC = 1`), so there is nothing to enable.
 *  - **Clustering the buildables is not mod-reachable.** `AActor::CanBeInCluster()` returns a member
 *    defaulting to false and only cooked level actors pass through `ULevel::CreateCluster()`;
 *    save-spawned buildables never do.
 *  - **FPM adds no churn of its own.** Grep for `NewObject|CreateWidget|SpawnActor|DuplicateObject`
 *    across this module returns **zero** hits. An absence claim needs its search shown, so that is the
 *    search.
 *
 * ⚠ AND IT STEERS NOTHING. GC pauses are deliberately excluded from anything the governor will later
 * act on, because no quality lever shortens a mark that scales with the live object graph. This meter
 * reports; it does not react.
 *
 * VIEWER ONLY: two engine delegates, no hook, no console-variable write, no ini.
 */
class FFPMGCMeter final : public IFPMFix
{
public:
	static FFPMGCMeter& Get();

	virtual const TCHAR* Name() const override { return TEXT("gc-meter"); }

	/**
	 * Both sides. The server's idle GC is already 10x slower by engine default
	 * (`GTimeBetweenPurgingPendingKillObjectsOnIdleServerMultiplier = 10.0f`), which makes the server a
	 * genuinely different measurement rather than a duplicate one — and the 560 ms save stall lives
	 * there too, so knowing whether a GC pass overlaps it is worth having.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * UnknownCause. The pauses are understood mechanically; what is NOT known is the split between
	 * timer-driven and forced, and the whole value of this fix is producing that number. Claiming
	 * anything stronger before the boot would be the drift the enum exists to stop.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::GCMeter; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** `FPM.GC.Report` — the session's pass count, the timer/forced split, and the worst pause. */
	static void ReportNow();
};
