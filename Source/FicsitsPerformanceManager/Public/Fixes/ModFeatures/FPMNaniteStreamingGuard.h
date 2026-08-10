// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMDiag.h"
#include "Core/FPMFixContract.h"

/**
 * ★ IS NANITE DROPPING GEOMETRIC DETAIL BECAUSE ITS STREAMING POOL IS FULL? MEASURED: NO.
 *
 * This shipped on 2026-08-10 as a GUARD that would raise the pool, and was demoted the same evening to
 * a METER by its own first measurement. The history is kept because the negative result is the valuable
 * part, and because re-proposing the raise without re-measuring would waste the boot that killed it.
 *
 * ══ THE HYPOTHESIS IT WAS BUILT ON ══
 *
 * Ant, 2026-08-03: *"its a streaming issue of some sort. my gpu isnt maxed and the trains mesh went low
 * poly as i loaded new terrain. something is still starved."*
 *
 * Nanite drops detail on purpose when its pool is overcommitted. The engine's own comment above the
 * controls (`NaniteStreamingManager.cpp:129`): *"Controls for dynamically adjusting quality (pixels per
 * edge) when the streaming pool is being overcommitted... can happen when rendering scenes with lots of
 * unique geometry at high resolutions."* A megabase is exactly that scene, so the fit looked strong:
 *
 *     :138  QualityScale.MaxPoolPercentage   85.0   scale DOWN above this pool load
 *     :130  QualityScale.MinPoolPercentage   70.0   scale UP below it
 *     :146  QualityScale.MinQuality          0.3    "1.0 disables any scaling"
 *
 * And the scaler is asymmetric (`:1193-1226`) — over budget for 2 frames multiplies by 0.97 ("adjust
 * quality down rapidly"), recovery needs 30 consecutive good frames first, then climbs 1% a frame. So a
 * one-second burst of page requests while terrain streams in would cause a drop lasting several
 * seconds. That is a clean mechanism for a visible artefact, and it is why this was worth building.
 *
 * ══ ⚠ AND IT DOES NOT HAPPEN. THE RAISE WAS REMOVED 2026-08-10. ══
 *
 * Measured in Ant's own base, from `FPM.Nanite.Report`:
 *
 *     nanite: pool 50 MB ... 1101 sample(s), 0 of them scaled down, 0.0 s below 1.0
 *
 * **1101 samples, and the factor never once left 1.00.** Nanite is not dropping geometric detail for
 * pool pressure on this game. "The trains mesh went low poly" therefore has some OTHER cause, and the
 * strongest remaining lead is the instancing/LOD lazy-load path — the distance-field audit measured the
 * missing-DF component count RISING as she played, which is that path failing to settle.
 *
 * The raise, the VRAM sizing and the `[512, 2048]` clamp are gone. The measurement stays, because it is
 * what proved the negative and it is what would catch this being different on weaker hardware.
 *
 * ⚠ THE BASELINE THE RAISE COMPARED AGAINST WAS WRONG ANYWAY, which is its own lesson. It measured
 * against the ENGINE default of 512 MB. **This game runs 50.** CSS set
 * `r.Nanite.Streaming.StreamingPoolSize=50` in their cooked `FactoryGame/Config/DefaultEngine.ini:47`,
 * directly under `:45 DynamicallyGrowAllocations=1` — a small seed with growth enabled. Whether that
 * growth is WHY the factor never drops is a HYPOTHESIS this meter cannot settle: the cvar would not move
 * even if the allocation grew underneath it.
 *
 * ══ WHY FPM1'S KEYS WERE A DIFFERENT LEVER AGAIN ══
 *
 * FPM1 wrote `r.Nanite.Streaming.MaxPageInstallsPerFrame` and `.MaxPendingPages` to `Engine.ini` and
 * spent a residue exception on them, because both are `ECVF_ReadOnly`. Those change how FAST pages
 * arrive. Neither changes pool size, which is what the quality scaler actually reads. FPM2 does not
 * carry that exception.
 *
 * ══ ⚠ IT STILL OWNS THE VRAM NUMBER THE TEXTURE POOL GUARD DEPENDS ON ══
 *
 * `FPMTexturePoolGuard` subtracts a Nanite reservation before sizing the texture pool, and that
 * reservation used to be the literal constant 512 — **wrong by 10x on this game**. Its own comment names
 * the mechanism: *"Nanite's floor, subtracted BEFORE textures. It fails globally at 85% occupancy;
 * textures fail gradually and locally."* That 85% is `QualityScale.MaxPoolPercentage`.
 *
 * `ReservedMB()` is the single declaration site and reads the LIVE cvar, so the texture pool sizes
 * itself against reality rather than an engine default this game never uses.
 *
 * ⚠ THE FLOOR CLAMP INSIDE `ComputePoolMB` STAYS, and now has a reason rather than an accident:
 * `DynamicallyGrowAllocations=1` means Nanite's real VRAM use grows past the 50 MB the cvar states, so
 * reserving only 50 would UNDER-reserve and let textures claim memory Nanite is about to take. Holding
 * the floor at 512 over-reserves deliberately, which is the safe direction of that error.
 *
 * ZERO RESIDUE, TRIVIALLY: it holds no cvar and writes nothing at all. It reads one float on a ticker.
 * CLIENT ONLY: a dedicated server renders no Nanite.
 */
class FFPMNaniteStreamingGuard final : public IFPMFix
{
public:
	static FFPMNaniteStreamingGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("nanite-streaming-meter"); }

	/** A dedicated server has no renderer, so it has no Nanite streaming pool to starve. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * UnknownCause, downgraded from OriginNamed on 2026-08-10. The mechanism this named — quality scaling
	 * under pool pressure — was MEASURED NOT TO OCCUR here, so claiming a named origin would be exactly
	 * the drift this enum exists to stop. What actually drops the geometry is still open.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::NaniteStreaming; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/**
	 * ★ THE SINGLE DECLARATION SITE for how much VRAM Nanite's streaming pool is configured with.
	 *
	 * Returns the LIVE cvar value, falling back to the engine default only when it cannot be read at all.
	 * `FPMTexturePoolGuard` calls this instead of carrying its own constant, so the two cannot disagree
	 * about how much of the card is already spoken for.
	 */
	static int32 ReservedMB();

	/** `FPM.Nanite.Report` — the live scale factor, the minimum ever seen, and what that means. */
	static void LogReport(class FOutputDevice* Ar = nullptr);
};
