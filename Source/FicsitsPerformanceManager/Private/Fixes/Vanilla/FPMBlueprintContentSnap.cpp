// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Vanilla/FPMBlueprintContentSnap.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"
#include "Fixes/Vanilla/FPMBlueprintContentSnapMode.h"

#include "Hologram/FGBlueprintHologram.h"
#include "FGBlueprintProxy.h"
#include "Buildables/FGBuildable.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGRailroadTrackConnectionComponent.h"

#include "Components/SceneComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

/*
 * v1 has no anchor cache (design doc S9's LRU cache is not built - header comment states why, S12 Q3 is
 * unruled). Cost is O(our open connectors x target open connectors) EVERY FRAME while the mode is active.
 * This is the tripwire that says so on a large blueprint instead of shipping a silent hitch.
 */
static TAutoConsoleVariable<float> CVarBlueprintContentSnapBudgetMs(
	TEXT("FPM.BlueprintContentSnap.BudgetMs"), 2.0f,
	TEXT("Warn (throttled) when one in-mode scan for a mate takes longer than this many milliseconds. "
	     "v1 has no anchor cache, so cost is O(our open connectors x target open connectors) every frame "
	     "while the mode is active. Default 2.0."),
	ECVF_Default);

namespace
{
	/** Best candidate pair found so far across all three connector families. */
	struct FPMBCSBestCandidate
	{
		bool bFound = false;
		float BestScoreSq = TNumericLimits<float>::Max();
		FVector OurLocation = FVector::ZeroVector;
		FVector OurNormal = FVector::ForwardVector;
		FVector TargetLocation = FVector::ZeroVector;
		FVector TargetNormal = FVector::ForwardVector;
	};
}

/**
 * Resolves the hit result to a blueprint proxy: either the proxy's own bounding box was hit directly
 * (`ShouldBuildGunHitProxies`), or a real buildable belonging to one was hit (`GetBlueprintProxy()`,
 * public, `FGBuildable.h`). A lightweight-instance hit resolves to neither and is treated as no target —
 * a named v1 limitation (A1 anchors only ever come from real actors' connection components, so a
 * lightweight-only hit could never have contributed one anyway).
 */
static AFGBlueprintProxy* FPMBCSResolveTargetProxy(const FHitResult& Hit)
{
	AActor* HitActor = Hit.GetActor();
	if (HitActor == nullptr) { return nullptr; }

	if (AFGBlueprintProxy* ProxyActor = Cast<AFGBlueprintProxy>(HitActor))
	{
		return ProxyActor;
	}
	if (AFGBuildable* HitBuildable = Cast<AFGBuildable>(HitActor))
	{
		return HitBuildable->GetBlueprintProxy();
	}
	return nullptr;
}

/**
 * OUR side of a candidate pair. `Buildable` is a real actor loaded into the hologram's private preview
 * world (a key of the PUBLIC `mBuildableToNewRoot`); `VisualRoot` is the scene component that map keys
 * point to, standing in for where that buildable will land in the game world once placed. This computes
 * the connector's pose relative to its own buildable, then reapplies that as a relative offset onto
 * `VisualRoot`'s CURRENT world transform.
 *
 * ⚠ EXECUTION-UNPROVEN, header-flagged: this assumes `VisualRoot` faithfully carries the buildable's
 * local rotation, not just its position. Boot-verification checklist item 3.
 */
static void FPMBCSGetOurAnchorWorldPose(const AFGBuildable* Buildable, const USceneComponent* VisualRoot,
	const FVector& ConnLocation, const FVector& ConnNormal, FVector& OutWorldLocation, FVector& OutWorldNormal)
{
	const FTransform BuildableWorld = Buildable->GetActorTransform();
	const FTransform ConnWorld(ConnNormal.Rotation(), ConnLocation);
	const FTransform ConnLocalToBuildable = ConnWorld * BuildableWorld.Inverse();
	const FTransform ConnWorldNow = ConnLocalToBuildable * VisualRoot->GetComponentTransform();
	OutWorldLocation = ConnWorldNow.GetLocation();
	OutWorldNormal = ConnWorldNow.GetRotation().Vector();
}

/**
 * One connector family's scan, both sides. `ConnT` is one of `UFGFactoryConnectionComponent`,
 * `UFGPipeConnectionComponentBase` (catches Hyper too — `GetComponents<T>` matches subclasses) or
 * `UFGRailroadTrackConnectionComponent`. `IsCompatible` is vanilla's OWN compatibility rule for that
 * family (`CanSnapTo` or `CanConnectTo` — see the header for why two different vanilla methods), never a
 * re-derived one. Score is squared distance from the hit point to the TARGET anchor — design S7.4's rule:
 * aim picks the edge.
 */
template<typename ConnT, typename CompatFn>
static void FPMBCSScanFamily(AFGBlueprintHologram* Self, const TArray<AFGBuildable*>& TheirBuildables,
	const FHitResult& Hit, CompatFn IsCompatible, FPMBCSBestCandidate& Best)
{
	for (const auto& OurPair : Self->mBuildableToNewRoot)
	{
		AFGBuildable* OurBuildable = OurPair.Key;
		USceneComponent* OurRoot = OurPair.Value;
		if (OurBuildable == nullptr || OurRoot == nullptr) { continue; }

		TArray<ConnT*> OurConns;
		OurBuildable->GetComponents(OurConns);
		for (ConnT* OurConn : OurConns)
		{
			if (OurConn == nullptr || OurConn->IsConnected()) { continue; }

			for (AFGBuildable* TheirBuildable : TheirBuildables)
			{
				if (TheirBuildable == nullptr) { continue; }

				TArray<ConnT*> TheirConns;
				TheirBuildable->GetComponents(TheirConns);
				for (ConnT* TheirConn : TheirConns)
				{
					if (TheirConn == nullptr || TheirConn->IsConnected()) { continue; }
					if (!IsCompatible(OurConn, TheirConn)) { continue; }

					const FVector TheirLoc = TheirConn->GetConnectorLocation();
					const float ScoreSq = FVector::DistSquared(Hit.Location, TheirLoc);
					if (ScoreSq < Best.BestScoreSq)
					{
						FVector OurLoc, OurNormal;
						FPMBCSGetOurAnchorWorldPose(OurBuildable, OurRoot,
							OurConn->GetConnectorLocation(), OurConn->GetConnectorNormal(), OurLoc, OurNormal);

						Best.bFound = true;
						Best.BestScoreSq = ScoreSq;
						Best.OurLocation = OurLoc;
						Best.OurNormal = OurNormal;
						Best.TargetLocation = TheirLoc;
						Best.TargetNormal = TheirConn->GetConnectorNormal();
					}
				}
			}
		}
	}
}

/**
 * Design S7.5. Rotates the hologram about Z ONLY (pitch/roll untouched — blueprints are upright, and
 * the hologram itself can never be pitched/rolled by the player anyway) so our connector's normal ends
 * up anti-parallel to the target's, then translates in full 3D so the connector POSITIONS coincide.
 */
static void FPMBCSSolveMatedTransform(const FTransform& SelfTransform,
	const FVector& OurConnLocation, const FVector& OurConnNormal,
	const FVector& TargetConnLocation, const FVector& TargetConnNormal,
	FTransform& OutNewSelfTransform)
{
	const float CurrentYaw = OurConnNormal.Rotation().Yaw;
	const float DesiredYaw = (-TargetConnNormal).Rotation().Yaw;
	const float DeltaYaw = FMath::FindDeltaAngleDegrees(CurrentYaw, DesiredYaw);

	const FQuat DeltaRotation(FRotator(0.f, DeltaYaw, 0.f));
	const FVector OffsetFromPivot = OurConnLocation - SelfTransform.GetLocation();
	const FVector RotatedOffset = DeltaRotation.RotateVector(OffsetFromPivot);

	const FRotator OldRotation = SelfTransform.Rotator();
	const FRotator NewRotation(OldRotation.Pitch, FRotator::NormalizeAxis(OldRotation.Yaw + DeltaYaw), OldRotation.Roll);
	const FVector NewLocation = TargetConnLocation - RotatedOffset;

	OutNewSelfTransform = FTransform(NewRotation, NewLocation, SelfTransform.GetScale3D());
}

FFPMBlueprintContentSnap& FFPMBlueprintContentSnap::Get()
{
	static FFPMBlueprintContentSnap Instance;
	return Instance;
}

bool FFPMBlueprintContentSnap::TryComputeContentSnap(AFGBlueprintHologram* Self, const FHitResult& Hit, FTransform& OutTransform)
{
	if (Self == nullptr) { return false; }

	AFGBlueprintProxy* TargetProxy = FPMBCSResolveTargetProxy(Hit);
	if (TargetProxy == nullptr || !TargetProxy->AreProxyBuildingsRegisteredAndValid())
	{
		// Expected during ordinary free-aim (nothing under the cursor yet) and during a client's brief
		// mid-replication window right after a proxy first becomes visible — both fall back to vanilla.
		++NoProxyFrames;
		return false;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const TArray<AFGBuildable*>& TheirBuildables = TargetProxy->GetBuildables();

	FPMBCSBestCandidate Best;
	FPMBCSScanFamily<UFGFactoryConnectionComponent>(Self, TheirBuildables, Hit,
		[](UFGFactoryConnectionComponent* Ours, UFGFactoryConnectionComponent* Theirs) { return Ours->CanSnapTo(Theirs); },
		Best);
	FPMBCSScanFamily<UFGPipeConnectionComponentBase>(Self, TheirBuildables, Hit,
		[](UFGPipeConnectionComponentBase* Ours, UFGPipeConnectionComponentBase* Theirs) { return Ours->CanSnapTo(Theirs); },
		Best);
	FPMBCSScanFamily<UFGRailroadTrackConnectionComponent>(Self, TheirBuildables, Hit,
		[](UFGRailroadTrackConnectionComponent* Ours, UFGRailroadTrackConnectionComponent* Theirs) { return Ours->CanConnectTo(Theirs); },
		Best);

	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	if (ElapsedMs > CVarBlueprintContentSnapBudgetMs.GetValueOnAnyThread())
	{
		++BudgetExceeded;
		UE_CLOG(BudgetExceeded % FPMLog::ThrottleNotable == 1, LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] blueprint content snap: scan took %.2f ms (budget %.2f ms). v1 has no anchor "
			     "cache (design S9) - this blueprint's connector count is starting to cost real frame "
			     "time. FPM.BlueprintContentSnap.BudgetMs raises the threshold if this is expected."),
			ElapsedMs, CVarBlueprintContentSnapBudgetMs.GetValueOnAnyThread());
	}

	if (!Best.bFound)
	{
		// Expected for a blueprint whose only contents are connection-less (a road pack under
		// foundation-lattice-only tiling) - v1's documented A1-only boundary, not a fault.
		++NoCandidateFrames;
		return false;
	}

	FPMBCSSolveMatedTransform(Self->GetActorTransform(), Best.OurLocation, Best.OurNormal,
		Best.TargetLocation, Best.TargetNormal, OutTransform);
	++SnapsApplied;
	return true;
}

void FFPMBlueprintContentSnap::Arm()
{
	if (GetSupportedBuildModesHandle.IsValid() || TrySnapToActorHandle.IsValid()) { return; }

	AFGBlueprintHologram* Sample = GetMutableDefault<AFGBlueprintHologram>();

	// NAME THE LAMBDAS FIRST - FPMHookLedger.h: SML's SUBSCRIBE_ macros split on top-level commas, and
	// naming the lambda moves its body entirely outside the macro invocation.
	auto OnGetSupportedBuildModes = [](auto& Scope, const AFGBlueprintHologram* Self,
		TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& OutBuildModes)
	{
		// Vanilla's three modes populate the array FIRST - additive by construction, they cannot be
		// dropped by appending after.
		Scope(Self, OutBuildModes);
		OutBuildModes.AddUnique(UFPMBlueprintContentSnapBuildMode::StaticClass());
		++FFPMBlueprintContentSnap::Get().ModeOffered;
	};

	auto OnTrySnapToActor = [](auto& Scope, AFGBlueprintHologram* Self, const FHitResult& Hit)
	{
		// Vanilla runs FIRST and its result is the default: if we find nothing, Scope's own result
		// (never overridden below) is exactly what gets returned - vanilla's box/centre snap or free
		// placement, untouched.
		Scope(Self, Hit);
		if (Self == nullptr) { return; }
		if (!Self->IsCurrentBuildMode(UFPMBlueprintContentSnapBuildMode::StaticClass())) { return; }

		FFPMBlueprintContentSnap& This = FFPMBlueprintContentSnap::Get();
		++This.InModeFrames;

		FTransform SnappedTransform;
		if (This.TryComputeContentSnap(Self, Hit, SnappedTransform))
		{
			Self->SetActorLocationAndRotation(SnappedTransform.GetLocation(), SnappedTransform.GetRotation());
			Scope.Override(true);   // contract: no further location/rotation writes this frame

			if (FPMDiag::IsOn(FPMDiag::EChannel::BlueprintContentSnap))
			{
				UE_LOG(LogFicsitsPerformanceManager, Verbose,
					TEXT("[FPM] blueprint content snap: mated at %s"),
					*SnappedTransform.GetLocation().ToString());
			}
		}
	};

	GetSupportedBuildModesHandle = FPM_SUBSCRIBE_VIRTUAL("blueprint-content-snap",
		AFGBlueprintHologram::GetSupportedBuildModes_Implementation, Sample, OnGetSupportedBuildModes);
	TrySnapToActorHandle = FPM_SUBSCRIBE_VIRTUAL("blueprint-content-snap",
		AFGBlueprintHologram::TrySnapToActor, Sample, OnTrySnapToActor);

	if (GetSupportedBuildModesHandle.IsValid() && TrySnapToActorHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] blueprint content snap ARMED. Hook A offers 'Blueprint (Content Snap)' alongside "
			     "vanilla's three blueprint build modes - additive, none of them touched. Hook B mates "
			     "open belt/pipe/rail connectors between the aimed blueprint's real contents and the one "
			     "being placed, ONLY while that mode is current; outside it vanilla's placement runs "
			     "untouched. v1 has no foundation-lattice (A2) anchor and no anchor cache - design doc "
			     "S12 Q3 and S9 are both open."));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] blueprint content snap NOT armed - hook install FAILED (mode-hook=%s, "
			     "snap-hook=%s). Blueprint placement runs entirely vanilla this session."),
			GetSupportedBuildModesHandle.IsValid() ? TEXT("ok") : TEXT("FAILED"),
			TrySnapToActorHandle.IsValid() ? TEXT("ok") : TEXT("FAILED"));
	}
}

void FFPMBlueprintContentSnap::Disarm()
{
	LogReport();

	// UNSUBSCRIBE_METHOD is correct for a _VIRTUAL subscribe - both drive the same HookInvoker, and
	// RemoveHandler uninstalls the detour once both the before- and after-maps are empty
	// (NativeHookManager.h). Guarded on IsValid() because the editor path installs nothing.
	if (TrySnapToActorHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(AFGBlueprintHologram::TrySnapToActor, TrySnapToActorHandle);
		TrySnapToActorHandle.Reset();
	}
	if (GetSupportedBuildModesHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(AFGBlueprintHologram::GetSupportedBuildModes_Implementation, GetSupportedBuildModesHandle);
		GetSupportedBuildModesHandle.Reset();
	}
}

void FFPMBlueprintContentSnap::LogReport()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] blueprint content snap report: mode offered %d time(s), %d frame(s) spent in-mode, "
		     "%d snap(s) applied, %d frame(s) with no target proxy, %d frame(s) with no compatible open "
		     "connector, %d scan(s) over budget."),
		ModeOffered, InModeFrames, SnapsApplied, NoProxyFrames, NoCandidateFrames, BudgetExceeded);

	// The liveness statement, same discipline as FPMCVarWriter's self-test: say what a zero MEANS rather
	// than let it read as "fine".
	if (ModeOffered == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   Hook A never fired - GetSupportedBuildModes_Implementation was never queried "
			     "this session. Expected if no blueprint hologram was ever spawned; if one WAS spawned "
			     "and this is still 0, Hook A did not install."));
	}
	else if (InModeFrames == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   the mode was offered but never selected - nobody cycled to it this session."));
	}
	else if (SnapsApplied == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   the mode was selected and aimed (%d frame(s)) but never found a compatible open "
			     "connector to mate against - expected against a blueprint with no A1 contents (v1's "
			     "documented boundary), or nobody aimed at a registered proxy yet."),
			InModeFrames);
	}
}

// WithOutputDevice - see FPMConsoleEcho.h - so the report prints in the console the player is looking at.
static FAutoConsoleCommandWithOutputDevice GBlueprintContentSnapReportCmd(
	TEXT("FPM.BlueprintContentSnap.Report"),
	TEXT("Print how many times the content-snap build mode was offered, entered, and successfully mated "
	     "a connector pair this session."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMBlueprintContentSnap::Get().LogReport();
	}));
