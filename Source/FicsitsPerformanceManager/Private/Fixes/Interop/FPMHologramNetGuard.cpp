// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMHologramNetGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "FGAttachmentPoint.h"
#include "Buildables/FGBuildable.h"
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
	 * Process-lifetime by design: these count hook fires, not per-world work, so the TOTALS are correct
	 * across a second world load — unlike the old mod's sweep counters, which re-printed the first
	 * world's numbers as if they were the second's.
	 *
	 * ⚠ BUT THE THROTTLE IS NOT TOTALS, AND THE FIRST VERSION OF THIS COMMENT OVERSTATED IT by saying
	 * "there is nothing here that a second world load should reset". The `N == 1` arm fires once per
	 * PROCESS, so a second world load in the same session prints no first-sighting line and the periodic
	 * arm lands at whatever offset the first load left behind. The counts stay true; the READOUT is
	 * quieter than it looks on load two. Left as-is deliberately — the totals are what settle the
	 * hypotheses below, and resetting per world would break exactly that — but stated, because a comment
	 * claiming more than the code does is the defect class this project keeps paying for.
	 */
	std::atomic<int32> GRepaired{0};
	std::atomic<int32> GIntact{0};
	std::atomic<int32> GCancelled{0};

	/*
	 * The fourth bucket, and the one that proves the narrowing works: a VANILLA class whose rebuild
	 * yielded nothing, which is normal and must fall through. If this is large and GCancelled is ~0, the
	 * split is right. If GCancelled climbs on FactoryGame class names, the origin test is wrong and the
	 * skip is too wide again.
	 */
	std::atomic<int32> GVanillaNoPoints{0};
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
		 * CHEAPEST TEST FIRST — but not for the reason the first version claimed.
		 *
		 * ⚠ IT DOES NOT "REJECT ESSENTIALLY EVERYTHING". That was written as if this ran on a server. It
		 * does not: this fix is client-only, and on a CLIENT most replicated actors ARE
		 * ROLE_SimulatedProxy, so the role compare passes often and the `IsA` cast behind it is what
		 * actually does the narrowing. The ordering is still right — an enum compare before an RTTI cast
		 * is strictly cheaper — but the justification was wrong, and a wrong reason is what stops the next
		 * reader noticing when the ordering stops being right.
		 *
		 * ROLE_SimulatedProxy is precisely "an observer's network-received copy"; the local player's own
		 * hologram is spawned locally and is never this.
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
		 * ★ THE REPAIR — AND v0.2.0 GOT IT WRONG IN EXACTLY THE WAY THE RAIN FIX DID.
		 *
		 * That version read `Hologram->GetComponents<UFGAttachmentPointComponent>()` — components on the
		 * HOLOGRAM. They are not there. Vanilla's own routine is
		 * `AFGBuildable::CreateAttachmentPointsFromComponents` (FGBuildable.cpp:2539-2566 — REAL code,
		 * not a link stub), and it reads two places, neither of them the hologram:
		 *
		 *     out_points.Empty();
		 *     ... AFGDecorationTemplate::GetComponentsFromSubclass<UFGAttachmentPointComponent>( GetDecorationTemplate() )
		 *     for( auto comp : TInlineComponentArray< UFGAttachmentPointComponent* >{ this } )  // `this` = BUILDABLE
		 *     ... ( Usage == EAPU_HologramOnly && owner->IsA< AFGHologram >() )                 // `owner` only FILTERS
		 *
		 * SETTLED FROM ASSET BYTES, not from reading source: of 400 exported assets containing
		 * `FGAttachmentPointComponent`, 350 are `Deco_*` DECORATION TEMPLATES and nearly all the rest are
		 * decorators too (`MSS_Deco_*`, `DC_Deco`, `CircuitryDefaultDecorator`). The components live on
		 * the deco template, reachable only through the buildable. The old code searched a set that is
		 * empty for essentially every class in the game, so every fire would have fallen through to the
		 * residual cancel below — shipping the exact regression this fix exists to remove, while the
		 * header claimed "Expected: zero".
		 *
		 * ⚠ SAME MISTAKE AS RAIN: right intent, right hook, WRONG OBJECT. The header even cited rain as
		 * the lesson while repeating it. Reading the correct object is the lesson; citing it is not.
		 *
		 * SO: CALL VANILLA'S OWN ROUTINE ON THE CDO OF THE CLASS THIS GHOST IS PREVIEWING.
		 *  - `GetBuildClass()` is public FORCEINLINE (FGHologram.h:271) and `mBuildClass` is
		 *    UPROPERTY(Replicated) (:756), so a simulated proxy genuinely knows what it is previewing.
		 *  - Using the CDO also fixes the TIMING half. A class default object's subobjects and its
		 *    `mDecoratorClass` (FGBuildable.h:846, read via the public GetDecorationTemplate() at :495)
		 *    exist before any BeginPlay, whereas the hologram's own components are copied from the
		 *    buildable INSIDE BeginPlay by SetupComponents (FGHologram.h:633-636). The old code ran
		 *    before its own inputs existed.
		 *  - Vanilla's three-way usage test is applied against `owner`, so passing the Hologram yields
		 *    Default + HologramOnly and correctly drops BuildableOnly. The hand-rolled filter is gone —
		 *    it was argued from the enum's NAME rather than from the code that consumes it.
		 */
		const TSubclassOf<AActor> BuildClass = Hologram->GetBuildClass();
		if (const AFGBuildable* BuildableCDO =
				BuildClass ? Cast<AFGBuildable>(BuildClass->GetDefaultObject()) : nullptr)
		{
			// Vanilla Empty()s out_points itself, so it owns the array outright — do not pre-clear.
			BuildableCDO->CreateAttachmentPointsFromComponents(Hologram->mCachedAttachmentPoints, Hologram);
		}

		if (Hologram->mCachedAttachmentPoints.Num() > 0)
		{
			// Fall through: BeginPlay runs, components begin play, the ghost renders, the assert holds.
			const int32 N = ++GRepaired;
			if ((N == 1 || (N % FPMLog::ThrottleRoutine) == 0)
			&& FPMDiag::IsOn(FPMDiag::EChannel::HologramNet))
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
		 * ⚠⚠ THE RESIDUAL CANCEL IS NARROWED TO MODDED CLASSES, AND THAT IS NOT A DETAIL.
		 *
		 * Caught in self-review after the data-source fix above: "the rebuild produced nothing" is the
		 * NORMAL, CORRECT outcome for most of the game. Only 400 exported assets carry an attachment-point
		 * component at all, so the overwhelming majority of buildings legitimately have none. Cancelling on
		 * an empty result would therefore skip BeginPlay for nearly every vanilla ghost — the exact
		 * regression Ant reported ("sometimes the holograms never rendered when sunfry held them in front
		 * of me"), reintroduced one layer further down. Fixing the source and leaving this here would have
		 * shipped the same bug with a better excuse.
		 *
		 * VANILLA WITH NO POINTS IS SAFE. Vanilla ships those classes and singleplayer does not assert on
		 * them; an empty cache is simply what they have. The assert belongs to a MOD
		 * (ModularStations' carousel) that requires points and never checks. So the honest split is by
		 * ORIGIN, not by emptiness:
		 *   - FactoryGame class, no points  -> normal. Fall through. The preview renders.
		 *   - modded class, no points       -> the carousel shape. Skip BeginPlay rather than hand a
		 *                                      joining client an assert, because a join crash rebinds a
		 *                                      player to a fresh character and costs an inventory.
		 *
		 * ⚠ HYPOTHESIS, STATED AS ONE: that no VANILLA hologram asserts on an empty cache. It follows from
		 * those classes shipping in a game that works, but it has not been exercised here. The Warning
		 * below names every skipped class precisely so one boot can settle it.
		 */
		const UPackage* Package = Hologram->GetClass()->GetOutermost();
		const FString PackageName = Package ? Package->GetName() : FString();
		const bool bVanilla = PackageName.StartsWith(TEXT("/Script/FactoryGame"))
			|| PackageName.StartsWith(TEXT("/Game/"));

		if (bVanilla)
		{
			/*
			 * Falls through — vanilla behaviour, the preview renders. But it must SAY SO periodically,
			 * because this is the branch the whole narrowing rests on and a silent counter cannot settle
			 * a hypothesis. Caught in self-review: the first draft incremented and returned, so one boot
			 * would have produced no evidence either way.
			 */
			const int32 N = ++GVanillaNoPoints;
			if ((N == 1 || (N % FPMLog::ThrottleRoutine) == 0)
			&& FPMDiag::IsOn(FPMDiag::EChannel::HologramNet))
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] hologram-net: %s is a VANILLA class with no attachment points (#%d) - "
					     "normal, falling through so the preview renders. repaired=%d intact=%d skipped=%d"),
					*Actor->GetClass()->GetName(), N, GRepaired.load(), GIntact.load(), GCancelled.load());
				FPMOverlay::Post(TEXT("hologram-net"),
					FString::Printf(TEXT("repaired %d · intact %d · vanilla-no-points %d · SKIPPED %d"),
						GRepaired.load(), GIntact.load(), N, GCancelled.load()));
			}
			return;
		}

		Scope.Cancel();

		const int32 N = ++GCancelled;

		/*
		 * Gated like everything else. The COUNTER still climbs when silenced, so FPM.Diag.List can
		 * still report it after the fact — which is the property that lets "0 = silent" be honest
		 * without losing the measurement.
		 */
		if (!FPMDiag::IsOn(FPMDiag::EChannel::HologramNet)) { return; }

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
