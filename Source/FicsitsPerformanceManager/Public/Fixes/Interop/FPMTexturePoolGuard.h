// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"
#include "Containers/Ticker.h"

/**
 * ★ THE TEXTURE-POOL GUARD — sizes `r.Streaming.PoolSize` from the card instead of leaving it at
 * vanilla's flat 1000 MB.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ MEASURED IN ANT'S GAME, 2026-08-09, before a line of this was written. Same session, same scene,
 * one cvar changed by hand in the console:
 *
 *     pool 1000    FPS 57   1% low 46   GPU 83%   168 W   LAT 18.5 / 83.9 ms   21 hitches / 60 s
 *     raised       FPS 89   1% low 44   GPU 99%   222 W   LAT 14.1 / 53.0 ms   12 hitches / 60 s
 *     raised more  FPS 92   1% low 69   GPU 99%   222 W   LAT 13.5 / 53.2 ms    7 hitches / 60 s
 *
 * READ THE GPU COLUMN, NOT THE FPS COLUMN. Utilisation and power rose WITH framerate (83%→99%,
 * 168→222 W). A GPU-limited game sits pinned at 99%; at 83% it was IDLE, waiting for textures that were
 * not resident. The pool was not costing quality, it was costing throughput.
 *
 * The two raises bought different things, and the port must not confuse them: the FIRST recovered the
 * AVERAGE (57→89), the SECOND bought CONSISTENCY (1% low 44→69, hitches 12→7) with the average already
 * pinned. A halfway pool fixes the framerate and leaves the stutter, so this ships the full computed
 * value rather than a timid one.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ⚠ THIS IS A REPAIR, NOT A PORT. THE OLD IMPLEMENTATION NEVER RAN.
 *
 * `Private/Governor/FPMTexturePoolGuard.cpp` in the old mod computes a `Ceiling` correctly and then:
 *     :150   int64 Claim = 0;               // declared
 *     :162   Out.TargetMB = (int32)Claim;   // never assigned in between — ALWAYS 0
 * and its driver reads 0 as "stand down", calls `Stop()`, and removes its own ticker. So 45 seconds
 * after every boot the guard computed zero, logged, switched itself off, and never wrote the cvar once.
 * The monotonic raise underneath was unreachable.
 *
 * WORSE, IT NAMED A FALSE CAUSE: the stand-down line reads "card below the guard tier". Ant's card is
 * 16303 MB against a 6000 MB tier. The one log line anybody would have checked asserted the opposite of
 * the truth, which is why this survived so long — and why the rule below exists.
 *
 * The old HEADER also documents two designs that were never implemented: a static
 * `max(Total - reserves, floor)` claim, and a demand-driven grower (`GrowStepMB`, `GrowAtOccupancy`).
 * Neither is in the code. This file implements ONE model and documents only that one.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ THE MODEL: PRIORITY ORDER, AND TEXTURES ARE LAST. Ant's ruling, 2026-07-25, preserved verbatim
 * because it IS the design:
 *
 *     "nanite is important that it always works. a blurry texture somewhere is better than a whole
 *      model flipping lods close to the player."
 *
 * Those two failures are not equivalent. A small texture pool costs SHARPNESS, gradually, and Epic
 * documents the pool as a soft budget. A starved Nanite pool trips a GLOBAL detail throttle at 85%
 * occupancy and makes geometry flip at every distance. So every consumer that fails harder is served
 * first and textures receive the remainder:
 *
 *     1. Nanite geometry floor    fails globally, at all distances
 *     2. Cartograph's allowance   a third-party mod that will allocate whether we like it or not
 *     3. Renderer working set     render targets and the post chain; scales with RESOLUTION, so it is
 *                                 a floored share rather than a flat number
 *     4. Headroom                 deliberately unclaimed — other mods, and being wrong
 *     5. TEXTURES                 whatever survives
 *
 * It scales by construction. On a small card the subtractions dominate and textures get little; on a
 * large one there is genuinely more left. No hardcoded ceiling — Ant rejected that outright: "the
 * ceiling would be different on a 24gb card and a 8gb card. cant hardcode that."
 *
 * ★ SIZED FROM TOTAL VRAM, NEVER FROM FREE. Total never breathes, so the answer is computed once and
 * never moves. Both dynamic designs of 2026-07-20 failed the same evening in opposite directions: a
 * ratchet that climbed to 8.4 GB, and a chaser that resized seven times in 82 seconds — and THAT was
 * the texture pop. A texture pool wants one size, decided once.
 *
 * ★ MONOTONIC. Ant: "the vram should allocate up and stay at the maximum it needs, so it can allocate
 * more, but not resize down." We only ever RAISE. The single repeat customer is the game's own
 * scalability pass, which re-applies `r.Streaming.PoolSize` when the player touches texture settings and
 * silently shrinks the pool back; correcting that clobber is the entire job of the slow watchdog.
 *
 * ★ NEVER WORSE THAN VANILLA. If the arithmetic says textures should get less than the game already
 * gives them, we write nothing at all. Writing a SMALLER number than vanilla chose would be a
 * regression wearing our name.
 *
 * ★ THE STAND-DOWN RULE, which the old version violated and is the reason it went unnoticed for months:
 * if this guard declines to act, it logs the ACTUAL failing predicate WITH its measured inputs — card
 * size, tier, computed ceiling, live pool. It never names a cause it has not tested.
 */
class FFPMTexturePoolGuard final : public IFPMFix
{
public:
	static FFPMTexturePoolGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("texture-pool-guard"); }

	/** A dedicated server has no renderer, no texture streaming, and nothing to size. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * Guard. The cause is not ours: vanilla ships a flat 1000 MB at TextureQuality@3 regardless of card,
	 * and Cartograph's map render target competes for the same memory. We prevent the harm; we did not
	 * create it and cannot fix it at source.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::TexturePool; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** `FPM.Diag.Dump` and `FPM.Support` print these. A guard that has never fired must look like one. */
	static void GetCounts(int32& OutRaises, int32& OutClobbersRepaired, int32& OutLastTargetMB);

private:
	bool Tick(float DeltaTime);

	FTSTicker::FDelegateHandle TickHandle;
	double StartTime  = 0.0;
	double LastPoll   = 0.0;
	bool   bDetected  = false;
	bool   bCartograph = false;
	bool   bStoodDown = false;   // latched, and always with a stated reason
};

/** The arithmetic, separated from the driver so it is testable and so it logs its own inputs. */
struct FFPMPoolDecision
{
	int32 TargetMB      = 0;      // 0 = write nothing, the game keeps its own value
	int32 CeilingMB     = 0;
	int32 NaniteMB      = 0;
	int32 CartographMB  = 0;
	int32 RendererMB    = 0;
	int32 HeadroomMB    = 0;
	bool  bCartograph   = false;
	const TCHAR* StandDownReason = nullptr;   // non-null ⇒ we declined, and this says exactly why
};

namespace FPMTexturePool
{
	/** Total dedicated VRAM in MB via the RHI (cross-platform; no DXGI, because the static model never
	 *  needs Budget or Free — and not needing them is what keeps the answer from moving). 0 if unready. */
	int64 QueryTotalVramMB();

	/** Is Cartograph loaded? It only decides whether an allowance is carved out, not whether we act. */
	bool DetectCartograph();

	/** Pure. No writes, no logging, no globals — so the caller can log every input beside the output. */
	FFPMPoolDecision ComputePoolMB(int64 VramMB, bool bCartographPresent);
}
