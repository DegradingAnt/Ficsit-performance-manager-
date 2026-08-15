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
 *  - **`gc.AllowIncrementalGather` IS A DEAD CVAR.** Added 2026-08-15. It exists, it is settable, and
 *    it changes nothing: its only consumer is `GarbageCollection.cpp:5676`, which also tests
 *    `FGCFlags::IsIncrementalGatherUnreachableSupported()`, and that is a hardcoded `return false;`
 *    (GarbageCollectionInternalFlags.h:143-146 — one definition in the whole engine, no `#if`
 *    alternative). The gather is always stop-the-world. Writing this would have been a confident no-op.
 *  - **`gc.IncrementalGatherTimeLimit` IS COMPILED OUT OF SHIPPING** (GarbageCollection.cpp:311-321:
 *    `#if UE_BUILD_SHIPPING` makes it a `constexpr 0.0f` with no cvar registration). It is present in
 *    Development, which is what FPM builds — so probing it from a dev build reports a lever the retail
 *    client and the DatHost server do not have.
 *  - **The engine's biggest GC forcer is already off, and Coffee Stain turned it off.**
 *    `s.ForceGCAfterLevelStreamedOut` (engine default 1, CoreSettings.cpp:25) makes World.cpp:4943-4948
 *    fire a FULL PURGE every time a level goes pending-purge — the literal "hitch when moving fast"
 *    mechanism. `Config/DefaultEngine.ini:495` sets it False, and `:502` sets
 *    `s.ContinuouslyIncrementalGCWhileLevelsPendingPurge` False, with CSS's own comment at :501 naming
 *    the hitch. That road has been walked; do not re-propose it.
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

	/**
	 * `FPM.GC.Report` — the session's pass count, the worst pause, the timer/forced GUESS, and the
	 * full-purge-vs-ordinary READING with a separate mean for each.
	 *
	 * ★ THE TWO SPLITS ARE NOT THE SAME QUESTION, and the second one is the one that decides anything.
	 * Timer-vs-forced is inferred from arrival time and asks "would a pacing lever have skipped this?".
	 * Full-purge-vs-ordinary is read from engine state and asks "was this pass four phases more
	 * expensive?" — see the block comment on `GFPMGCFullPurges` in the .cpp for the verified chain.
	 * They are independent, so the report also counts where they disagree.
	 *
	 * ★ SECTION 7.3, THE UOBJECT WATERMARK RIDES ALONG IN THIS SAME REPORT. `m6164470` (every FPM
	 * feature reports to the dev overlay) is satisfied by the sticky gauge row `Arm()` sets up; the
	 * printed percentage here is the query surface. `FPM.Status` is not touched by this file -
	 * Slice 4's host-probe work scoped that command narrowly to the tier line on purpose (its own
	 * design note), so a general status surface is a separate, later item, not this one silently
	 * growing past its stated job.
	 */
	static void ReportNow();

	/**
	 * ★ SECTION 7.3, THE UOBJECT WATERMARK. `GetObjectArrayNumMinusAvailable()` already gave this
	 * meter the live claimed-object count; this is the other half, the CEILING it is measured
	 * against. `GUObjectArray.GetObjectArrayCapacity()` (`UObjectArray.h:1225-1228`, engine source)
	 * returns `ObjObjects.Capacity()`, the array's fixed `MaxElements`, set once at
	 * `FUObjectArray::AllocateObjectPool()` and never grown. That is a genuine crash ceiling: the
	 * process dies when the array fills, it does not degrade, because every one of this game's 151
	 * mods registers objects into the SAME array.
	 *
	 * ★ CREDIT AND THE DISCLOSURE THAT TRAVELS WITH IT (design section 7.3). The technique, watch
	 * UObject count against capacity as a crash predictor, is established by `Th3UObjectCounter`
	 * (Th3Fanbus/Rex, via FRM), and FRM's own docs disclose that AN INSTRUMENT THAT CREATES OBJECTS
	 * TO COUNT OBJECTS PERTURBS ITS OWN MEASUREMENT. Stated honestly rather than silently claiming a
	 * cleaner number: FPM's read does NOT create anything - `GetObjectArrayNumMinusAvailable()` and
	 * `GetObjectArrayCapacity()` are both direct reads of counters the engine already maintains,
	 * reachable here because this is native C++ with engine access FRM's own approach evidently did
	 * not have. The disclosure is carried forward as context for the technique's LINEAGE, not
	 * because this specific implementation repeats the flaw it describes.
	 *
	 * @return {claimed object count, capacity, percentage}. Percentage is 0 if capacity reads 0
	 *         (never divides by zero; that would itself be a finding worth a warning, not a crash).
	 */
	struct FWatermark
	{
		int32 Claimed = 0;
		int32 Capacity = 0;
		float Percent = 0.f;
	};
	static FWatermark GetWatermark();

	/** Percentage at or above this prints a warning and a sticky overlay row, every time it is
	 *  crossed on a read - not throttled, because a crash ceiling this close is worth repeating
	 *  over "an event believed unreachable is logged unthrottled" (FPMFixContract.h's own policy
	 *  for findings that matter more than log volume). Design's own number (§7.3). */
	static constexpr float WatermarkWarningPercent = 85.f;
};
