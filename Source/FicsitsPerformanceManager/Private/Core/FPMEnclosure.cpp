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

	/** Cadence cap while moving. The effects downstream fade over ~0.5 s, so 4 Hz is already generous. */
	constexpr double GFPMMinIntervalSec = 0.25;

	/** Symmetric damping. 3 readings each way — see the header for why neither direction gets a break. */
	constexpr int32 GFPMStreakToFlip = 3;

	/** Sealed-room threshold, and a roof threshold. Both unmeasured; they become knobs when a boot says so. */
	constexpr float GFPMSealedMin = 0.55f;
	constexpr float GFPMOverheadMin = 0.60f;

	/** At least this many distinct rays, so one big nearby surface cannot carry a fraction alone. */
	constexpr int32 GFPMMinHits = 3;

	struct FFPMConsumer
	{
		const TCHAR* Name = nullptr;
		EFPMEnclosureNeed Need = EFPMEnclosureNeed::Overhead;
		bool bActive = false;
	};

	TArray<FFPMConsumer> GConsumers;
	int32 GActiveConsumers = 0;

	/** The unit directions, built once. */
	TArray<FVector> GDirections;

	/** In-flight batch state. Only touched on the game thread. */
	struct FFPMBatch
	{
		TArray<uint8> RayResult;   // 0 = miss, 1 = world hit, 2 = built hit
		TArray<float> RayDistance;
		int32 Returned = 0;
		bool bInFlight = false;
		FVector Origin = FVector::ZeroVector;
	};
	FFPMBatch GBatch;

	FTraceDelegate GTraceDelegate;
	bool GDelegateBound = false;

	FFPMEnclosureReading GLast;
	FVector GLastProbeOrigin = FVector(FLT_MAX);
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

		// Symmetric streak damping, applied to both verdicts independently.
		const bool bRoofRaw = (R.BuiltHits >= GFPMMinHits) && (R.BuiltOverhead >= GFPMOverheadMin);
		if (bRoofRaw) { ++GRoofStreak; GNoRoofStreak = 0; } else { ++GNoRoofStreak; GRoofStreak = 0; }
		if (!GbUnderRoof && GRoofStreak >= GFPMStreakToFlip) { GbUnderRoof = true; }
		else if (GbUnderRoof && GNoRoofStreak >= GFPMStreakToFlip) { GbUnderRoof = false; }

		const bool bSealRaw = (R.BuiltHits >= GFPMMinHits) && (R.BuiltSealed >= GFPMSealedMin);
		if (bSealRaw) { ++GSealStreak; GNoSealStreak = 0; } else { ++GNoSealStreak; GSealStreak = 0; }
		if (!GbSealed && GSealStreak >= GFPMStreakToFlip) { GbSealed = true; }
		else if (GbSealed && GNoSealStreak >= GFPMStreakToFlip) { GbSealed = false; }

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

		if (++GBatch.Returned >= GDirections.Num())
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
	GBatch.bInFlight = false;
	GbUnderRoof = GbSealed = false;
}

bool FPMEnclosure::IsUnderBuiltRoof() { return GbUnderRoof; }
bool FPMEnclosure::IsInSealedRoom() { return GbSealed; }
const FFPMEnclosureReading& FPMEnclosure::Last() { return GLast; }

double FPMEnclosure::SecondsSinceReading()
{
	return GLastCompleteTime > 0.0 ? FPlatformTime::Seconds() - GLastCompleteTime : TNumericLimits<double>::Max();
}

namespace
{
bool TickInternal(float /*DeltaSeconds*/)
{
	// 1. NOTHING LISTENING, NOTHING RUNS. Zero cost, not merely low cost.
	if (GActiveConsumers <= 0 || GBatch.bInFlight) { return true; }

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
	if (GLast.bValid && FVector::DistSquared(Origin, GLastProbeOrigin)
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

	for (int32 i = 0; i < GDirections.Num(); ++i)
	{
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
}

static FAutoConsoleCommand GFPMEnclosureReportCmd(
	TEXT("FPM.Enclosure.Report"),
	TEXT("Print the shared indoor reading, both verdicts, and what the sampling has cost."),
	FConsoleCommandDelegate::CreateStatic(&FPMEnclosure::LogNow));
