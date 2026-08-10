// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMDiag.h"
#include "Core/FPMFixContract.h"

/**
 * ★ "THE TRAINS MESH WENT LOW POLY AS I LOADED NEW TERRAIN" — MEASURED, THEN FIXED, AND NOT WITH AN INI.
 *
 * Ant, 2026-08-03: *"its a streaming issue of some sort. my gpu isnt maxed and the trains mesh went low
 * poly as i loaded new terrain. something is still starved."*
 *
 * ══ WHAT IS ACTUALLY HAPPENING, WITH THE ENGINE'S OWN WORDS FOR IT ══
 *
 * Nanite drops geometric detail on purpose when its streaming pool is overcommitted. The comment above
 * the controls says so (`NaniteStreamingManager.cpp:129`):
 *
 *     "Controls for dynamically adjusting quality (pixels per edge) when the streaming pool is being
 *      overcommitted. This should be a rare condition in practice, but can happen when rendering scenes
 *      with lots of unique geometry at high resolutions."
 *
 * A Satisfactory megabase is precisely "lots of unique geometry". The numbers, read this session:
 *
 *     :134  r.Nanite.Streaming.StreamingPoolSize                 512 MB   ECVF_RenderThreadSafe
 *     :138  r.Nanite.Streaming.QualityScale.MaxPoolPercentage    85.0     scale DOWN above this load
 *     :130  r.Nanite.Streaming.QualityScale.MinPoolPercentage    70.0     scale UP below this load
 *     :146  r.Nanite.Streaming.QualityScale.MinQuality           0.3      "1.0 disables any scaling"
 *
 * So: pool goes over 85% full, quality walks down toward 0.3, and the train goes low poly. "My GPU isn't
 * maxed" fits exactly — this is not a throughput problem, it is a 512 MB pool being too small for the
 * scene. That is a different mechanism from the one FPM1 went after.
 *
 * ★ AND THE SCALER IS DELIBERATELY ASYMMETRIC, WHICH IS WHY A BRIEF SPIKE LEAVES A LASTING ARTEFACT.
 * `FQualityScalingManager::Update` (`NaniteStreamingManager.cpp:1193-1226`), engine bytes:
 *
 *     over budget 2 frames running   ->  Scale *= 0.97   // "adjust quality down rapidly"
 *     under budget 30 frames running ->  Scale *= 1.01   // "slowly start increasing quality again"
 *
 * Falling 1.0 -> 0.3 takes about forty frames. Climbing back needs thirty consecutive good frames
 * before it even begins, then about a hundred and twenty more at one percent each. **Loading new terrain is
 * exactly a short burst of page requests** — a transient overcommit that costs under a second to cause
 * and several seconds to undo. That asymmetry is the difference between a number moving and Ant seeing
 * a low-poly train, and it is also why a one-second sample interval is enough to catch it.
 *
 * ══ WHY FPM1'S KEYS WERE THE WRONG LEVER, AND WHY THIS NEEDS NO INI ══
 *
 * FPM1 wrote `r.Nanite.Streaming.MaxPageInstallsPerFrame` and `.MaxPendingPages` to `Engine.ini`,
 * because both are `ECVF_ReadOnly` and there is no runtime path to them. Those raise how FAST pages
 * arrive and how many may be in flight. Neither changes how BIG the pool is, and pool size is what the
 * quality scaler reads.
 *
 * `r.Nanite.Streaming.StreamingPoolSize` is `ECVF_RenderThreadSafe` with no `ECVF_ReadOnly`, so it is
 * writable at runtime. FPM2 therefore fixes the reported symptom with a normal cvar hold and writes
 * nothing that survives an uninstall. **The ini exception FPM1 spent here was not needed.**
 *
 * ══ IT MEASURES BEFORE IT ACTS, AND KEEPS MEASURING AFTER ══
 *
 * `Nanite::FStreamingManager::GetQualityScaleFactor()` (`NaniteStreamingManager.h:88-91`) returns the
 * live scale — 1.0 means no scaling, anything below means Nanite is dropping detail RIGHT NOW. It is a
 * public header-inline accessor on `Nanite::GStreamingManager` (`:361`), so reading it needs no hook, no
 * access transformer, and carries no risk of a missing symbol.
 *
 * That turns the whole feature into an experiment that reports its own result:
 *
 *   1. Sample the factor on a slow ticker. Record the minimum and how long it stayed below 1.0.
 *   2. The FIRST time scaling is seen, raise the pool once — and only then. A machine that never
 *      overcommits never gets a write, so the feature costs nothing where it is not needed.
 *   3. Keep sampling. The report says whether the minimum improved AFTER the raise.
 *
 * ★ Step 3 is the part that makes this more than a guess at a number. If the pool goes up and the
 * factor still walks down, the raise did not work and the report says so instead of implying success
 * from the fact that a write happened.
 *
 * ⚠ THE SAMPLE IS A DELIBERATELY UNSYNCHRONISED FLOAT READ. `QualityScaleFactor` is written by the
 * streaming manager's own update, not by the game thread. This reads it without a lock, and that is
 * correct rather than sloppy: an aligned four-byte load cannot tear, the value is a diagnostic and not
 * a control input, and taking a lock the render thread holds every frame to read one float would cost
 * far more than the reading is worth. What it can do is return a value one update stale. It cannot
 * return garbage.
 *
 * ⚠ AND THE SAMPLE CAN MISS A DIP SHORTER THAN ITS INTERVAL. Stated because it bounds what a clean
 * report proves: "min 1.0" means "never caught below 1.0 at a sample point", not "never scaled".
 *
 * ══ ⚠ IT SHARES VRAM WITH THE TEXTURE POOL GUARD, AND THE TWO MUST NOT DRIFT ══
 *
 * `FPMTexturePoolGuard` subtracts a Nanite reservation from the card before sizing the texture pool, and
 * that reservation was the literal constant 512 — the same number as this pool's engine default. Its
 * own comment names the reason and, read today, names this exact mechanism a year early:
 * *"Nanite's floor, subtracted BEFORE textures. It fails globally at 85% occupancy; textures fail
 * gradually and locally."* That 85% is `QualityScale.MaxPoolPercentage`.
 *
 * So raising this pool without raising that reservation would let the texture pool claim VRAM this one
 * is now using, and the two fixes would quietly contend. `ReservedMB()` below is the SINGLE declaration
 * site, and the texture pool guard reads it rather than holding its own copy. Two copies of one number
 * is a bug by construction, and this project has already paid for that once.
 *
 * ZERO RESIDUE: one hold through `FPMCVarWriter` on a `Module` lease. No ini.
 * CLIENT ONLY: a dedicated server renders no Nanite.
 */
class FFPMNaniteStreamingGuard final : public IFPMFix
{
public:
	static FFPMNaniteStreamingGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("nanite-streaming-guard"); }

	/** A dedicated server has no renderer, so it has no Nanite streaming pool to starve. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * The cause is named with a receipt and it is not the one FPM1 named: quality scaling driven by pool
	 * overcommit above `QualityScale.MaxPoolPercentage`, `NaniteStreamingManager.cpp:129-149`.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::NaniteStreaming; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/**
	 * ★ THE SINGLE DECLARATION SITE for how much VRAM Nanite's streaming pool is using.
	 *
	 * Returns the pool size FPM is currently holding, or the engine's own value when FPM holds nothing.
	 * `FPMTexturePoolGuard` calls this instead of carrying its own constant, so the two cannot disagree
	 * about how much of the card is already spoken for.
	 */
	static int32 ReservedMB();

	/** `FPM.Nanite.Report` — the live scale factor, the minimum seen, and whether the raise helped. */
	static void LogReport(class FOutputDevice* Ar = nullptr);
};
