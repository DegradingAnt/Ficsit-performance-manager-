// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMEnclosure.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"

#include "AbstractInstanceManager.h"      // ULightweightCollisionComponent — the walls that are not actors
#include "Buildables/FGBuildable.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "WorldCollision.h"

namespace
{
	/*
	 * THE RAY SET — 24 directions on a Fibonacci hemisphere.
	 *
	 * Even angular coverage with no axis bias. Hand-placed rings cannot give that, and it starts to
	 * matter the moment walls count as much as roofs: a ring layout leaves seams between the rings, and
	 * a doorway that lines up with a seam reads as sealed.
	 *
	 * ⚠ z >= kFloorCut EXCLUDES THE FLOOR ON PURPOSE. Straight down is blocked whenever the player is
	 * standing on anything at all, so it is a constant. A constant costs a trace, tells you nothing, and
	 * silently inflates every fraction computed over the set.
	 */
	constexpr int32 GFPMRayCount = 24;
	constexpr float GFPMFloorCut = -0.15f;

	/** A ray counts as "overhead" above this z. Used for the roof-only predicate. */
	constexpr float GFPMOverheadZ = 0.5f;

	/**
	 * Reach, cm. 60 m, and it is a TENSION rather than a preference.
	 *
	 * Too short and a tall hall's ceiling is out of range, so a covered player reads as exposed and the
	 * bug survives. Too long and the near-horizontal rays start hitting a big factory's OUTSIDE walls
	 * from outside it, so someone standing in the open beside their base reads as sealed and loses
	 * effects they should have.
	 */
	constexpr float GFPMTraceCm = 6000.f;

	/** Re-probe only after the camera has moved this far. See the header — this is the big saving. */
	constexpr float GFPMMoveThresholdCm = 150.f;

	/**
	 * ★ THE MOVEMENT SKIP IS A RATE REDUCER, NOT A CACHE. This is what stops it becoming one.
	 *
	 * Ant, 2026-08-10: *"what if another player builds something around the first player without the
	 * first moving? that would break the system."* It did. The skip below tested only whether the CAMERA
	 * had moved, and `GLast.bValid` never expires — so a player who stands still keeps the same answer
	 * forever, and the world is the one input that skip cannot see.
	 *
	 * Two ways to be wrong, and the second is worse than the first:
	 *   - SunFry walls you in while you stand still  -> the gate still believes you are outdoors, and
	 *     weather keeps playing indoors. The bug this whole fix exists to prevent.
	 *   - Someone DISMANTLES your shelter while you stand still -> the gate keeps suppressing weather and
	 *     you stand in the rain with no rain. Silently wrong, and it reads as the mod being broken.
	 *
	 * The comment on the skip said the answer "depends on POSITION and not on facing". True in a static
	 * world. Satisfactory is a game about building, so a static world is the one assumption never
	 * available here — and the claim was written as though it were unconditional.
	 *
	 * A maximum age bounds it with one constant and no new dependency. A stationary player re-probes at
	 * 0.5 Hz instead of 4 Hz, which is still an eightfold saving over the moving case, and the answer can
	 * now be stale for at most this long instead of for the rest of the session.
	 *
	 * ⚠ This is the FLOOR, not the whole answer. A build event can invalidate immediately and should —
	 * FPM already receives every `AFGBuildable::BeginPlay` (`FPMRainOcclusionFix.cpp:658`), so that path
	 * costs nothing to add. Dismantles have no such hook, which is exactly why the age cap has to exist
	 * underneath rather than being replaced by the event.
	 */
	constexpr double GFPMMaxCacheAgeSec = 2.0;

	/** Cadence cap while moving. The effects downstream fade over ~0.5 s, so 4 Hz is already generous. */
	constexpr double GFPMMinIntervalSec = 0.25;

	/**
	 * Symmetric damping. 3 readings each way — see the header for why neither direction gets a break.
	 *
	 * ★ MEASURED TOO WEAK, BUT NOT RETUNED BLIND. Ant's client log, 2026-08-11, while she was standing
	 * still inside her factory:
	 *     weather gate: sealed room = YES  11.04.43
	 *     weather gate: sealed room = no   11.04.46
	 *     weather gate: sealed room = YES  11.04.49
	 * Three flips in six seconds. At `GFPMMinIntervalSec` 0.25 s a streak of 3 can flip in under a
	 * second, so genuinely marginal geometry — a doorway, a conveyor hole, an open bay — oscillates.
	 * Each flip drives a hold/release on the weather parameters, which now reads as rain popping.
	 *
	 * ⚠ THE CONSTANT IS NOT BUMPED HERE, DELIBERATELY. What the right streak is depends on her actual
	 * building, and picking 5 or 8 from an armchair is the guessing this project keeps paying for. It is
	 * exposed as a cvar so ONE boot can answer it — raise it live, stand in the same spot, watch the flip
	 * lines stop — and the answer then becomes the new default with a measurement behind it.
	 */
	TAutoConsoleVariable<int32> CVarEnclosureStreakToFlip(
		TEXT("FPM.Enclosure.StreakToFlip"), 3,
		TEXT("Consecutive agreeing samples needed to flip the sealed/under-roof verdict. Higher = steadier "
		     "but slower to react. Default 3, measured to oscillate in a real factory; raise it and watch "
		     "the 'sealed room =' lines to find the value that holds."),
		ECVF_Default);

	/** Sealed-room threshold, and a roof threshold. Both unmeasured; they become knobs when a boot says so. */
	constexpr float GFPMSealedMin = 0.55f;
	constexpr float GFPMOverheadMin = 0.60f;

	/** At least this many distinct rays, so one big nearby surface cannot carry a fraction alone. */
	constexpr int32 GFPMMinHits = 3;

	/**
	 * ⚠ HOW LONG A BATCH MAY BE IN FLIGHT BEFORE IT IS ABANDONED.
	 *
	 * Found by review, and it was a BLOCKER rather than a tidy-up. `bInFlight` was cleared only when all
	 * 24 callbacks returned. Tear the world down mid-batch and the queued async traces are discarded, the
	 * callbacks never arrive, and the flag stays set -- so Tick early-returns FOREVER and the check that
	 * fog, particles and the visor gate all depend on is silently dead for the rest of the session, with
	 * nothing reporting it.
	 *
	 * Async traces are documented to complete on the NEXT frame (World.h:2377), so seconds is already
	 * enormously generous; anything past this did not merely run late, it is never coming.
	 */
	constexpr double GFPMBatchTimeoutSec = 5.0;

	/** Batches abandoned by that timeout. Non-zero means level transitions are interrupting the probe. */
	int32 GBatchesAbandoned = 0;

	struct FFPMConsumer
	{
		const TCHAR* Name = nullptr;
		EFPMEnclosureNeed Need = EFPMEnclosureNeed::Overhead;
		bool bActive = false;
	};

	TArray<FFPMConsumer> GConsumers;
	int32 GActiveConsumers = 0;

	/**
	 * ★ DOES ANY LIVE CONSUMER ACTUALLY NEED THE WALLS?
	 *
	 * Review finding: EFPMEnclosureNeed was stored, logged, and never read -- decorative surface, which is
	 * speculative generality. It earns its place here instead. Wall rays are only traced when something
	 * asks about a sealed room; a roof-only consumer (the visor gate) traces the upward rays alone, which
	 * is roughly half the set. In a performance mod, a parameter that does not change what runs should
	 * either start doing so or stop existing.
	 */
	bool GNeedSealed = false;

	void RecomputeNeeds()
	{
		GNeedSealed = false;
		for (const FFPMConsumer& C : GConsumers)
		{
			if (C.bActive && C.Need == EFPMEnclosureNeed::SealedRoom) { GNeedSealed = true; break; }
		}
	}

	/** The unit directions, built once. */
	TArray<FVector> GDirections;

	/** In-flight batch state. Only touched on the game thread. */
	struct FFPMBatch
	{
		TArray<uint8> RayResult;   // 0 = miss, 1 = world hit, 2 = built hit
		TArray<float> RayDistance;
		int32 Returned = 0;
		int32 Expected = 0;
		bool bInFlight = false;
		FVector Origin = FVector::ZeroVector;
	};
	FFPMBatch GBatch;

	FTraceDelegate GTraceDelegate;
	bool GDelegateBound = false;

	FFPMEnclosureReading GLast;
	FVector GLastProbeOrigin = FVector(FLT_MAX);

	/**
	 * How many times a nearby build forced an early re-probe. Reported beside the skip counter so the
	 * fast path can be seen working — a zero here with a busy build session means the wiring is dead,
	 * which is the shape this project keeps paying for.
	 */
	int32 GInvalidationsByBuild = 0;
	double GLastProbeTime = 0.0;
	double GLastCompleteTime = 0.0;

	int32 GRoofStreak = 0, GNoRoofStreak = 0, GSealStreak = 0, GNoSealStreak = 0;
	bool GbUnderRoof = false, GbSealed = false;

	/** Batches issued and rays traced this session, so the cost is reportable rather than asserted. */
	int32 GBatchesIssued = 0;
	int32 GBatchesSkippedStill = 0;

	/** Owned by the facility, so "nothing runs when nothing is listening" is structural. */
	FTSTicker::FDelegateHandle GTicker;

	bool TickInternal(float DeltaSeconds);

	void BuildDirections()
	{
		if (GDirections.Num() > 0) { return; }
		GDirections.Reserve(GFPMRayCount);

		/*
		 * Fibonacci lattice over the band z in [kFloorCut, 1]. The golden-angle step in azimuth is what
		 * gives even coverage without seams; a naive ring layout leaves gaps a doorway can hide in.
		 */
		const float GoldenAngle = PI * (3.f - FMath::Sqrt(5.f));
		for (int32 i = 0; i < GFPMRayCount; ++i)
		{
			const float T = (GFPMRayCount == 1) ? 1.f : static_cast<float>(i) / (GFPMRayCount - 1);
			const float Z = FMath::Lerp(GFPMFloorCut, 1.0f, T);
			const float R = FMath::Sqrt(FMath::Max(0.f, 1.f - Z * Z));
			const float Theta = GoldenAngle * i;
			GDirections.Emplace(R * FMath::Cos(Theta), R * FMath::Sin(Theta), Z);
		}
	}

	/**
	 * ★ IS THIS HIT A PLAYER BUILDABLE?
	 *
	 * ⚠ THE ACTOR CAST ALONE IS NOT ENOUGH, AND THIS IS THE DETAIL THE WHOLE FEATURE TURNS ON. Since
	 * Satisfactory 1.0 the ordinary foundations and walls are LIGHTWEIGHT INSTANCES — they have no
	 * actor, and their collision lives on a `ULightweightCollisionComponent`
	 * (`AbstractInstanceManager.h:251`, with a hierarchical sibling at `:262`). A probe that only cast
	 * the hit ACTOR to `AFGBuildable` would miss exactly the walls the player is standing behind, report
	 * open sky inside a sealed room, and look like a threshold problem.
	 */
	bool IsBuiltCover(const FHitResult& Hit)
	{
		if (const AActor* Actor = Hit.GetActor())
		{
			if (Actor->IsA<AFGBuildable>()) { return true; }
		}
		if (const UPrimitiveComponent* Comp = Hit.GetComponent())
		{
			if (Comp->IsA<ULightweightCollisionComponent>()
				|| Comp->IsA<ULightweightHierarchicalInstancedStaticMeshComponent>())
			{
				return true;
			}
		}
		return false;
	}

	void Finalise()
	{
		FFPMEnclosureReading R;
		R.bValid = true;

		int32 BuiltAll = 0, AnyAll = 0, OverheadTotal = 0, OverheadBuilt = 0;
		float Nearest = -1.f;

		for (int32 i = 0; i < GDirections.Num(); ++i)
		{
			const uint8 Result = GBatch.RayResult.IsValidIndex(i) ? GBatch.RayResult[i] : 0;
			const bool bOverhead = GDirections[i].Z >= GFPMOverheadZ;
			if (bOverhead) { ++OverheadTotal; }

			if (Result != 0)
			{
				++AnyAll;
				if (Result == 2)
				{
					++BuiltAll;
					if (bOverhead) { ++OverheadBuilt; }
					const float D = GBatch.RayDistance.IsValidIndex(i) ? GBatch.RayDistance[i] : -1.f;
					if (D >= 0.f && (Nearest < 0.f || D < Nearest)) { Nearest = D; }
				}
			}
		}

		const int32 N = FMath::Max(1, GDirections.Num());
		R.BuiltSealed = static_cast<float>(BuiltAll) / N;
		R.AnySealed = static_cast<float>(AnyAll) / N;
		R.BuiltOverhead = OverheadTotal > 0 ? static_cast<float>(OverheadBuilt) / OverheadTotal : 0.f;
		R.BuiltHits = BuiltAll;
		R.NearestCm = Nearest;

		GLast = R;
		GLastCompleteTime = FPlatformTime::Seconds();

		// Symmetric streak damping, applied to both verdicts independently. Read ONCE per batch so both
		// verdicts use the same threshold even if the cvar changes mid-flight.
		const int32 StreakToFlip = FMath::Max(1, CVarEnclosureStreakToFlip.GetValueOnAnyThread());

		const bool bRoofRaw = (R.BuiltHits >= GFPMMinHits) && (R.BuiltOverhead >= GFPMOverheadMin);
		if (bRoofRaw) { ++GRoofStreak; GNoRoofStreak = 0; } else { ++GNoRoofStreak; GRoofStreak = 0; }
		if (!GbUnderRoof && GRoofStreak >= StreakToFlip) { GbUnderRoof = true; }
		else if (GbUnderRoof && GNoRoofStreak >= StreakToFlip) { GbUnderRoof = false; }

		// Only meaningful when the wall band was actually traced. Without it the sealed fraction is
		// computed over the roof alone and would read as sealed inside any covered but open-sided area.
		const bool bSealRaw = GNeedSealed && (R.BuiltHits >= GFPMMinHits) && (R.BuiltSealed >= GFPMSealedMin);
		if (bSealRaw) { ++GSealStreak; GNoSealStreak = 0; } else { ++GNoSealStreak; GSealStreak = 0; }
		if (!GbSealed && GSealStreak >= StreakToFlip) { GbSealed = true; }
		else if (GbSealed && GNoSealStreak >= StreakToFlip) { GbSealed = false; }

		GBatch.bInFlight = false;

		if (FPMDiag::IsOn(FPMDiag::EChannel::Enclosure, 2))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] enclosure: built-sealed %.2f, built-overhead %.2f, any-sealed %.2f, "
				     "%d built hit(s), nearest %.0f cm -> roof=%s sealed=%s"),
				R.BuiltSealed, R.BuiltOverhead, R.AnySealed, R.BuiltHits, R.NearestCm,
				GbUnderRoof ? TEXT("yes") : TEXT("no"), GbSealed ? TEXT("yes") : TEXT("no"));
		}
	}

	void OnTraceDone(const FTraceHandle& /*Handle*/, FTraceDatum& Datum)
	{
		if (!GBatch.bInFlight) { return; }

		const int32 Index = static_cast<int32>(Datum.UserData);
		if (GBatch.RayResult.IsValidIndex(Index))
		{
			if (Datum.OutHits.Num() > 0)
			{
				const FHitResult& Hit = Datum.OutHits[0];
				GBatch.RayResult[Index] = IsBuiltCover(Hit) ? 2 : 1;
				GBatch.RayDistance[Index] = Hit.Distance;
			}
			else
			{
				GBatch.RayResult[Index] = 0;
			}
		}

		if (++GBatch.Returned >= GBatch.Expected)
		{
			Finalise();
		}
	}
}

int32 FPMEnclosure::Register(const TCHAR* ConsumerName, EFPMEnclosureNeed Need)
{
	FFPMConsumer C;
	C.Name = ConsumerName;
	C.Need = Need;
	C.bActive = true;
	++GActiveConsumers;

	if (!GTicker.IsValid())
	{
		// First consumer: the sampler starts here and nowhere else.
		GTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickInternal), 0.0f);
	}

	const int32 Token = GConsumers.Add(C);
	RecomputeNeeds();
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] enclosure: '%s' registered (%s). %d consumer(s) active - the sampler runs only while "
		     "at least one is."),
		ConsumerName, Need == EFPMEnclosureNeed::SealedRoom ? TEXT("sealed room") : TEXT("roof overhead"),
		GActiveConsumers);
	return Token;
}

void FPMEnclosure::Unregister(int32 Token)
{
	if (GConsumers.IsValidIndex(Token) && GConsumers[Token].bActive)
	{
		GConsumers[Token].bActive = false;
		--GActiveConsumers;
		RecomputeNeeds();
	}

	if (GActiveConsumers <= 0 && GTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
		GTicker.Reset();
	}
}

void FPMEnclosure::Shutdown()
{
	if (GTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTicker); GTicker.Reset(); }
	GConsumers.Reset();
	GActiveConsumers = 0;
	RecomputeNeeds();
	GBatch.bInFlight = false;
	GbUnderRoof = GbSealed = false;
}

bool FPMEnclosure::IsUnderBuiltRoof() { return GbUnderRoof; }
bool FPMEnclosure::IsInSealedRoom() { return GbSealed; }
const FFPMEnclosureReading& FPMEnclosure::Last() { return GLast; }

void FPMEnclosure::InvalidateNear(const FVector& WorldLocation, float RadiusCm)
{
	// Never probed yet: GLastProbeOrigin is FLT_MAX (:160) and the distance test below would overflow
	// into nonsense. Nothing to invalidate either way — the first probe has not happened.
	if (!GLast.bValid) { return; }

	if (FVector::DistSquared(WorldLocation, GLastProbeOrigin) > RadiusCm * RadiusCm) { return; }

	/*
	 * Expire the AGE, not the reading. Zeroing GLastProbeTime makes the max-age test in the tick fail on
	 * the next pass, which forces a re-probe. `GLast` itself is left intact so consumers keep answering
	 * with the previous verdict until the new one lands — see the header for why blanking it would
	 * flicker every downstream effect for no correctness gain.
	 */
	GLastProbeTime = 0.0;
	++GInvalidationsByBuild;
}

double FPMEnclosure::SecondsSinceReading()
{
	return GLastCompleteTime > 0.0 ? FPlatformTime::Seconds() - GLastCompleteTime : TNumericLimits<double>::Max();
}

namespace
{
bool TickInternal(float /*DeltaSeconds*/)
{
	// 1. NOTHING LISTENING, NOTHING RUNS. Zero cost, not merely low cost.
	if (GActiveConsumers <= 0) { return true; }

	if (GBatch.bInFlight)
	{
		// A batch whose callbacks are never coming must not wedge the sampler shut. See the timeout.
		if (FPlatformTime::Seconds() - GLastProbeTime < GFPMBatchTimeoutSec) { return true; }

		GBatch.bInFlight = false;
		++GBatchesAbandoned;
		UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Enclosure), LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] enclosure: abandoned a batch after %.0f s with %d of %d rays returned (%d total "
			     "abandoned). Usually a world teardown mid-batch. The sampler continues."),
			GFPMBatchTimeoutSec, GBatch.Returned, GBatch.Expected, GBatchesAbandoned);
	}

	UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
	if (World == nullptr || !World->IsGameWorld()) { return true; }

	APlayerController* PC = GEngine->GetFirstLocalPlayerController(World);
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (Pawn == nullptr)
	{
		// Menu, spectator or mid-respawn. Not a finding; there is simply nowhere to trace from.
		GLast.bValid = false;
		return true;
	}

	const FVector Origin = PC->PlayerCameraManager
		? PC->PlayerCameraManager->GetCameraLocation()
		: Pawn->GetActorLocation() + FVector(0, 0, 50.f);

	const double Now = FPlatformTime::Seconds();

	// 2. CADENCE CAP.
	if (Now - GLastProbeTime < GFPMMinIntervalSec) { return true; }

	/*
	 * 3. ★ THE BIG SAVING: DO NOT RE-PROBE A PLAYER WHO HAS NOT MOVED.
	 *
	 * The ray set is world-space, so the answer depends on POSITION and not on facing. Standing at a
	 * machine, turning on the spot, reading a sign — all reuse the last reading. That is the common
	 * case by a wide margin, which is why this is measure number one rather than a nicety.
	 */
	/*
	 * ⚠ AND THE ANSWER EXPIRES — but only when it CAN have changed. Ant, 2026-08-10, on the first
	 * version of this: *"but if it loops every 2 s then wont it lag the main thread?"*
	 *
	 * The age cap has exactly one job left, because builds are already event-driven through
	 * `InvalidateNear`: catching a DISMANTLE, which has no hook to ride. And a dismantle can only change
	 * the verdict for a player who is currently ENCLOSED — if the last reading said "outdoors", there is
	 * no roof and no wall to take away, and re-probing cannot produce a different answer.
	 *
	 * So the cap applies only to a POSITIVE cached verdict. A player standing in open terrain re-probes
	 * never. A player standing inside re-probes at 0.5 Hz. The cost lands exactly where a change is
	 * physically possible and nowhere else.
	 *
	 * On the cost itself, since that was the question: the traces are ASYNC onto the physics scene's own
	 * workers (see :474 and the AsyncLineTraceByChannel call below), nothing traces at all while no
	 * consumer is registered (:400), and the mod already runs at 4 Hz whenever the player is MOVING
	 * (`GFPMMinIntervalSec`). This is one eighth of that rate, in the one case it applies to.
	 */
	const bool bCachedVerdictPositive = GbUnderRoof || GbSealed;
	const bool bCacheStillFresh = !bCachedVerdictPositive
		|| (Now - GLastProbeTime) < GFPMMaxCacheAgeSec;

	if (GLast.bValid
		&& bCacheStillFresh
		&& FVector::DistSquared(Origin, GLastProbeOrigin)
			< GFPMMoveThresholdCm * GFPMMoveThresholdCm)
	{
		++GBatchesSkippedStill;
		return true;
	}

	BuildDirections();

	if (!GDelegateBound)
	{
		GTraceDelegate.BindStatic(&OnTraceDone);
		GDelegateBound = true;
	}

	GBatch.RayResult.Init(0, GDirections.Num());
	GBatch.RayDistance.Init(-1.f, GDirections.Num());
	GBatch.Returned = 0;
	GBatch.bInFlight = true;
	GBatch.Origin = Origin;
	GLastProbeOrigin = Origin;
	GLastProbeTime = Now;
	++GBatchesIssued;

	/*
	 * 4. ASYNC. AsyncLineTraceByChannel (World.h:2372) queues onto the physics scene's own workers and
	 * calls back on the game thread next frame. The game thread never blocks on a trace, which is the
	 * whole point of doing it this way in a mod whose job is frame time.
	 */
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FPMEnclosure), /*bTraceComplex*/ false, Pawn);
	Params.bReturnPhysicalMaterial = false;

	/*
	 * Only the rays a live consumer needs. With no sealed-room consumer the wall band is skipped
	 * entirely, which is about half the set -- and the batch's expected return count is adjusted to
	 * match, or Finalise would never fire.
	 */
	GBatch.Expected = 0;
	for (int32 i = 0; i < GDirections.Num(); ++i)
	{
		if (!GNeedSealed && GDirections[i].Z < GFPMOverheadZ) { continue; }
		++GBatch.Expected;
		World->AsyncLineTraceByChannel(EAsyncTraceType::Single, Origin,
			Origin + GDirections[i] * GFPMTraceCm, ECC_Visibility, Params,
			FCollisionResponseParams::DefaultResponseParam, &GTraceDelegate, static_cast<uint32>(i));
	}

	return true;
}
}

void FPMEnclosure::LogNow()
{
	const FFPMEnclosureReading& R = GLast;
	if (!R.bValid)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] enclosure: NO READING. %d consumer(s) active. Either nothing has registered, or "
			     "there is no local pawn to trace from. This is a statement about the instrument, not "
			     "about the room."),
			GActiveConsumers);
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] enclosure: built-sealed %.2f (>= %.2f seals) | built-overhead %.2f (>= %.2f roofs) | "
		     "any-sealed %.2f | %d built hits of %d rays | nearest %.0f cm"),
		R.BuiltSealed, GFPMSealedMin, R.BuiltOverhead, GFPMOverheadMin, R.AnySealed,
		R.BuiltHits, GDirections.Num(), R.NearestCm);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   verdict: under-roof=%s, sealed-room=%s. Reading is %.1f s old."),
		GbUnderRoof ? TEXT("YES") : TEXT("no"), GbSealed ? TEXT("YES") : TEXT("no"),
		SecondsSinceReading());

	/*
	 * The cost line is printed WITH the answer on purpose. This is a performance mod, and a diagnostic
	 * that reports what it found without reporting what it cost is half a diagnostic. The skipped count
	 * is the interesting one: it is how much the stationary-player check actually saved.
	 */
	const int32 Total = GBatchesIssued + GBatchesSkippedStill;
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   cost: %d batch(es) of %d ASYNC rays issued, %d skipped because the player had not "
		     "moved (%.0f%% of %d opportunities). Nothing traces while no consumer is registered."),
		GBatchesIssued, GDirections.Num(), GBatchesSkippedStill,
		Total > 0 ? 100.0 * GBatchesSkippedStill / Total : 0.0, Total);

	// The fast path's own liveness. A busy build session with zero here means the AFGBuildable::BeginPlay
	// wiring is dead and the max-age cap is silently carrying the whole correctness burden.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   %d early re-probe(s) forced by a build within reach of the player. Zero during a "
		     "building session would mean the build-event wiring is dead and only the %.0fs age cap is "
		     "protecting against being walled in while standing still."),
		GInvalidationsByBuild, GFPMMaxCacheAgeSec);

	UE_CLOG(GBatchesAbandoned > 0, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM]   %d batch(es) ABANDONED on timeout - callbacks never arrived, almost always a "
		     "world teardown mid-batch. Not fatal; the sampler recovers."), GBatchesAbandoned);
}

static FAutoConsoleCommand GFPMEnclosureReportCmd(
	TEXT("FPM.Enclosure.Report"),
	TEXT("Print the shared indoor reading, both verdicts, and what the sampling has cost."),
	FConsoleCommandDelegate::CreateStatic(&FPMEnclosure::LogNow));
