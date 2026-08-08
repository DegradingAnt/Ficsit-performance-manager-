// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMHologramNetGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMOverlay.h"

#include "FGAttachmentPoint.h"
#include "FGAttachmentPointComponent.h"
#include "GameFramework/Actor.h"
#include "Hologram/FGBuildableHologram.h"

#include <atomic>

namespace
{
	/*
	 * Session counters. Deliberately three, because the whole point of the rewrite is to know the SPLIT
	 * between them, and one aggregate number would hide it:
	 *   Repaired  — the cache was empty and we rebuilt it. BeginPlay ran. The ghost renders. (Expected: all.)
	 *   Intact    — the cache already had points. We did nothing. (Expected: some, harmless.)
	 *   Cancelled — repair produced nothing, so BeginPlay was skipped to save the joiner. (Expected: ZERO.)
	 *
	 * Process-lifetime by design: these count hook fires, not per-world work, so unlike the old mod's
	 * sweep counters there is nothing here that a second world load should reset. (That bug was real —
	 * file-static sweep totals re-printed the first world's numbers on the second load — so the
	 * distinction is stated rather than assumed.)
	 */
	std::atomic<int32> GRepaired{0};
	std::atomic<int32> GIntact{0};
	std::atomic<int32> GCancelled{0};
}

FFPMHologramNetGuard& FFPMHologramNetGuard::Get()
{
	static FFPMHologramNetGuard Instance;
	return Instance;
}

void FFPMHologramNetGuard::Arm()
{
	/*
	 * THE HOOK POINT IS UNCHANGED FROM THE OLD MOD, AND THAT IS DELIBERATE.
	 *
	 * AActor::DispatchBeginPlay is the only place that runs before EVERY BeginPlay body, including a
	 * subclass's. Hooking AFGHologram::BeginPlay instead would not help: SUBSCRIBE_METHOD_VIRTUAL patches
	 * only the class it is given, and AFGCarouselHologram overrides BeginPlay — so the mod's own body,
	 * the one that asserts, would run unpatched. The generic dispatcher is the correct choke point.
	 *
	 * It is also non-virtual ENGINE_API and large, so SUBSCRIBE_METHOD is the right macro and funchook
	 * has room to patch it.
	 */
	auto OnDispatchBeginPlay = [](auto& Scope, AActor* Actor, bool bFromLevelStreaming)
	{
		/*
		 * CHEAPEST TEST FIRST. This runs for every actor that ever begins play, so the role compare — an
		 * enum load and a branch that rejects essentially everything — is the gate, and the cast comes
		 * second. ROLE_SimulatedProxy is precisely "an observer's network-received copy"; the local
		 * player's own hologram is spawned locally and is never this.
		 */
		if (!Actor || Actor->GetLocalRole() != ROLE_SimulatedProxy) { return; }

		AFGBuildableHologram* Hologram = Cast<AFGBuildableHologram>(Actor);
		if (!Hologram) { return; }

		/*
		 * NARROWER THAN THE OLD FIX'S IsA<AFGHologram>() ON PURPOSE. mCachedAttachmentPoints is declared
		 * on AFGBuildableHologram (FGBuildableHologram.h:523), so a plain AFGHologram cannot have the
		 * problem this fix exists for, and had no business being caught by it.
		 */
		if (Hologram->mCachedAttachmentPoints.Num() > 0)
		{
			++GIntact;
			return; // already populated — vanilla behaviour untouched
		}

		/*
		 * THE REPAIR. Rebuild the cache the placement flow never ran, from the components the replicated
		 * actor already carries.
		 *
		 * BuildableOnly points are excluded because the enum says they are not for holograms
		 * (FGAttachmentPointComponent.h:11-16). Default and HologramOnly both belong here.
		 */
		TArray<UFGAttachmentPointComponent*> Points;
		Hologram->GetComponents<UFGAttachmentPointComponent>(Points);

		for (const UFGAttachmentPointComponent* Point : Points)
		{
			if (!Point || Point->GetAttachmentPointUsage() == EAttachmentPointUsage::EAPU_BuildableOnly)
			{
				continue;
			}
			Hologram->mCachedAttachmentPoints.Add(Point->CreateAttachmentPoint(Hologram));
		}

		if (Hologram->mCachedAttachmentPoints.Num() > 0)
		{
			// Fall through: BeginPlay runs, components begin play, the ghost renders, the assert holds.
			const int32 N = ++GRepaired;
			if (N == 1 || (N % 100) == 0)
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] hologram-net: rebuilt %d attachment point(s) on a replicated %s (#%d) - "
					     "BeginPlay now runs, so the remote build preview renders"),
					Hologram->mCachedAttachmentPoints.Num(), *Actor->GetClass()->GetName(), N);
				FPMOverlay::Post(TEXT("hologram-net"),
					FString::Printf(TEXT("%d ghost(s) repaired, %d already intact"), N, GIntact.load()));
			}
			return;
		}

		/*
		 * ⚠ RESIDUAL ONLY. No usable attachment-point component exists, so the cache cannot be rebuilt and
		 * a mod that asserts on it would take the joining client down — and a join crash is the mechanism
		 * that rebinds a player to a fresh character and loses their inventory.
		 *
		 * Skipping a preview is cosmetic. Losing a session is not. So the old behaviour survives here and
		 * ONLY here, and it names the class every single time (no throttle) because the expected count is
		 * zero and each occurrence is a finding rather than noise.
		 */
		Scope.Cancel();

		const int32 N = ++GCancelled;
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] hologram-net: %s (#%d) is a replicated hologram with NO usable attachment-point "
			     "component, so its cache cannot be rebuilt. Skipped BeginPlay to stop the assert that "
			     "kills a joining client - this preview will NOT render. If this names a vanilla "
			     "(FactoryGame) class, the skip is too wide and must be narrowed to modded classes."),
			*Actor->GetClass()->GetName(), N);
		FPMOverlay::Post(TEXT("hologram-net"),
			FString::Printf(TEXT("UNREPAIRABLE %s (#%d) - preview skipped"), *Actor->GetClass()->GetName(), N));
	};

	FPM_SUBSCRIBE("hologram-net", AActor::DispatchBeginPlay, OnDispatchBeginPlay);
}
