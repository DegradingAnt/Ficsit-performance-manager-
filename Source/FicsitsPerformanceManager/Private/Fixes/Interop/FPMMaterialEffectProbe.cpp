// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMMaterialEffectProbe.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "FGMaterialEffectComponent.h"

#include "Components/MeshComponent.h"
#include "GameFramework/Actor.h"

#include <atomic>

namespace
{
	/**
	 * ⚠ ATOMIC BECAUSE BUILD EFFECTS ARE NOT PROVABLY GAME-THREAD-ONLY HERE.
	 *
	 * The same reasoning `FFPMNoOwnerRpcGate` already applies to its own census: its filter is
	 * `IsA<AFGBuildable>` and buildables are what the multithreaded Factory Tick ticks. This probe has
	 * not proven which thread reaches `SetMeshes`, so the counter is atomic and the map is guarded by
	 * the game-thread check below rather than assumed safe.
	 */
	std::atomic<uint64> GSetMeshesTotal{0};

	/**
	 * Owner class path -> how many calls came from it. **GAME THREAD ONLY** — see the guard at the call
	 * site. A TMap is not thread-safe and this probe will not repeat the bug it exists to find.
	 */
	TMap<FString, int32> GMatEffectCallers;

	/** Empty-mesh-array calls, counted separately: a caller passing nothing is its own kind of wrong. */
	int32 GMatEffectEmptyCalls = 0;

	/** Calls whose component had no owning actor at all. */
	int32 GMatEffectOwnerlessCalls = 0;

	/**
	 * The census cap. Twenty distinct owner classes is far past the point where the pattern is visible,
	 * and the whole value is naming WHICH mount points appear, not enumerating every buildable.
	 */
	constexpr int32 GMatEffectOwnerLimit = 20;

	bool bGMatEffectCensusSaturated = false;

	FDelegateHandle GSetMeshesHandle;
}

FFPMMaterialEffectProbe& FFPMMaterialEffectProbe::Get()
{
	static FFPMMaterialEffectProbe Instance;
	return Instance;
}

void FFPMMaterialEffectProbe::Arm()
{
	if (GSetMeshesHandle.IsValid()) { return; }

	// NAME THE LAMBDA FIRST — sf-scaffold section 7. The body then never sits inside SML's macro, so a
	// top-level comma in it can never be split by the preprocessor.
	auto OnSetMeshes = [](auto& Scope, UFGMaterialEffectComponent* Self, TArray<UMeshComponent*> Meshes)
	{
		GSetMeshesTotal.fetch_add(1, std::memory_order_relaxed);

		/*
		 * ⚠ EVERYTHING BELOW IS GAME-THREAD ONLY, and the guard is the whole reason this is safe. A
		 * TMap::Add from two threads corrupts the map's storage. Losing a census entry on a worker costs
		 * a name; losing the map costs a crash inside the mod whose job is to stop those.
		 *
		 * ★ AND THE CALL ALWAYS FALLS THROUGH. No Scope.Cancel(), no mutation of Meshes. This probe must
		 * not change what the game does, because the whole point is to observe a behaviour nobody has
		 * explained yet.
		 */
		if (!IsInGameThread() || Self == nullptr) { return; }

		if (Meshes.Num() == 0) { ++GMatEffectEmptyCalls; }

		const AActor* Owner = Self->GetOwner();
		if (Owner == nullptr)
		{
			++GMatEffectOwnerlessCalls;
			return;
		}

		/*
		 * ★ THE CLASS PATH, NOT THE CLASS NAME. GetName() would give "BP_Foo_C" and leave the mod
		 * unidentified; GetPathName() gives the mount point it was loaded from — "/Script/FactoryGame..."
		 * for vanilla, "/SS_Mod/..." or another plugin root for a mod. The mount point IS the answer.
		 */
		const FString OwnerPath = Owner->GetClass()->GetPathName();

		if (int32* Existing = GMatEffectCallers.Find(OwnerPath))
		{
			++(*Existing);
			return;
		}

		if (GMatEffectCallers.Num() < GMatEffectOwnerLimit)
		{
			GMatEffectCallers.Add(OwnerPath, 1);

			UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::MaterialEffect),
				LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] material-effect: first SetMeshes from owner %s (component %s, %d mesh(es))"),
				*OwnerPath, *Self->GetClass()->GetName(), Meshes.Num());
		}
		else if (!bGMatEffectCensusSaturated)
		{
			/*
			 * ⚠ SAY IT ONCE RATHER THAN GOING QUIET. A census that stops at its cap without announcing it
			 * reads as complete coverage, and "these 20 classes are all of them" is exactly the wrong
			 * conclusion to hand someone chasing an unknown caller.
			 */
			bGMatEffectCensusSaturated = true;
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] material-effect: owner census SATURATED at %d distinct classes. Further "
				     "callers are counted in the total but NOT named. Raise the cap if the answer is not "
				     "already visible."), GMatEffectOwnerLimit);
		}
	};

	GSetMeshesHandle = FPM_SUBSCRIBE("material-effect-probe",
		UFGMaterialEffectComponent::SetMeshes, OnSetMeshes);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] material-effect probe ARMED - READ ONLY, never cancels. Vanilla logs "
		     "'SetMeshes, This cannot be called after PreStarted' 431 times in one of Ant's sessions "
		     "(FGMaterialEffectComponent.h:66 documents the contract), which means the meshes are never "
		     "set and the dismantle effect runs on an empty set. The caller is unknown because the .cpp "
		     "is a stub, so this names the OWNING ACTOR CLASS PATH of every call - the mount point in "
		     "that path identifies the mod. FPM.MaterialEffect.Report."));
}

void FFPMMaterialEffectProbe::Disarm()
{
	LogReport();

	/*
	 * Unsubscribed for the same reason the blueprint sweep gate is: a handler that keeps running past
	 * Disarm would keep writing GMatEffectCallers after the report claimed to be final. Guarded on
	 * IsValid() because the editor path returns an invalid handle and SML's arrays were never allocated.
	 */
	if (GSetMeshesHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UFGMaterialEffectComponent::SetMeshes, GSetMeshesHandle);
		GSetMeshesHandle.Reset();
	}
}

void FFPMMaterialEffectProbe::LogReport(FOutputDevice* Ar)
{
	const uint64 Total = GSetMeshesTotal.load(std::memory_order_relaxed);

	TArray<FString> Lines;
	Lines.Add(FString::Printf(
		TEXT("[FPM] material-effect: %llu SetMeshes call(s) seen, %d distinct owner class(es) named, "
		     "%d with an EMPTY mesh array, %d with no owning actor."),
		Total, GMatEffectCallers.Num(), GMatEffectEmptyCalls, GMatEffectOwnerlessCalls));

	if (Total == 0)
	{
		/*
		 * ★ THE DEAD-INSTRUMENT BRANCH. Zero calls does not mean the game never sets meshes — it means
		 * this hook never fired, and the two are indistinguishable without saying so. The input that
		 * would make it non-zero is nameable and cheap: dismantle anything.
		 */
		Lines.Add(TEXT("[FPM]   NO CALLS SEEN. Either the hook did not install, or nothing has run a "
		               "build/dismantle effect yet this session. Dismantle something and run this again - "
		               "a zero here is 'not measured', not 'never happens'."));
	}
	else
	{
		// Sorted by call count, because the noisy caller is the one worth naming first.
		GMatEffectCallers.ValueSort([](int32 A, int32 B) { return A > B; });

		int32 Shown = 0;
		for (const TPair<FString, int32>& Pair : GMatEffectCallers)
		{
			Lines.Add(FString::Printf(TEXT("[FPM]   %6d  %s"), Pair.Value, *Pair.Key));
			if (++Shown >= 12) { break; }
		}

		Lines.Add(TEXT("[FPM]   Read the MOUNT POINT of each path: '/Script/FactoryGame...' is vanilla, "
		               "anything else is the plugin root of the mod that owns the buildable. That is the "
		               "name the 'cannot be called after PreStarted' warning does not give you."));
	}

	for (const FString& L : Lines)
	{
		if (Ar != nullptr) { Ar->Log(L); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *L);
	}
}

static FAutoConsoleCommandWithOutputDevice GMaterialEffectReportCmd(
	TEXT("FPM.MaterialEffect.Report"),
	TEXT("Print which actor classes call UFGMaterialEffectComponent::SetMeshes, to identify the caller "
	     "behind vanilla's 'cannot be called after PreStarted' warnings."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMMaterialEffectProbe::LogReport(&Ar);
	}));
