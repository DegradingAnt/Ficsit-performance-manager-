// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMInstanceRemoveSwap.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"

// Owns ULightweightHierarchicalInstancedStaticMeshComponent. ABSTRACTINSTANCE_API-exported, so no
// access transformer is involved anywhere in this fix.
#include "AbstractInstanceManager.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/*
	 * Cumulative across the session, not per-sweep. A per-sweep counter would reset before anyone read
	 * it, which is the bug the rain sweep's own counters had and had to be fixed for.
	 */
	int32 GFPMSwapConverted = 0;      // components this fix actually had to change
	int32 GFPMSwapAlreadySet = 0;     // components already correct when we reached them
	int32 GFPMSwapSweeps = 0;
	int32 GFPMSwapLastChanged = -1;   // -1 = no sweep yet; distinguishes "none needed" from "never ran"

	/**
	 * When to re-sweep after a world load, in seconds. Buildables keep spawning as the world streams in
	 * and as the player builds, so a single load-time pass would cover only what already existed.
	 *
	 * Deliberately a short ladder rather than a permanent ticker: the CDO write below probably covers
	 * new components outright, and if it does these later sweeps report zero and cost one actor
	 * iteration each. If it does NOT, this is the evidence — see the header's note on why that return
	 * value is the experiment.
	 */
	const float GFPMSwapResweepSec[] = { 15.f, 60.f, 240.f };
	constexpr int32 GFPMSwapResweepCount = static_cast<int32>(UE_ARRAY_COUNT(GFPMSwapResweepSec));
	int32 GFPMSwapResweepIndex = 0;

	/*
	 * THE BEHAVIOUR SWITCH, separate from FPM.Diag.InstanceSwap which only changes what is printed.
	 *
	 * 0 makes this fix OBSERVE: it counts how many components carry the wrong removal semantics and
	 * changes none of them. That is what makes an A/B possible without a rebuild, and it is how the
	 * claim "this is what causes invisible-but-solid" gets tested rather than asserted.
	 */
	TAutoConsoleVariable<int32> CVarInstanceSwapEnabled(
		TEXT("FPM.InstanceSwap.Enabled"), 1,
		TEXT("Set RemoveAtSwap on AbstractInstance's mesh components so their removal matches the handle "
		     "table's. 1 = repair (default), 0 = count them and change nothing."),
		ECVF_Default);
}

FFPMInstanceRemoveSwap& FFPMInstanceRemoveSwap::Get()
{
	static FFPMInstanceRemoveSwap Instance;
	return Instance;
}

void FFPMInstanceRemoveSwap::Arm()
{
	/*
	 * ★ THE CDO WRITE — cheap, probably sufficient, and explicitly NOT TRUSTED.
	 *
	 * Setting the flag on the class default object should mean every component constructed afterwards
	 * starts with the correct removal semantics, because UE builds a new object from its archetype's
	 * memory. But `bSupportRemoveAtSwap` is a plain `uint8 : 1` with no UPROPERTY
	 * (InstancedStaticMeshComponent.h:234), so that inheritance is an implementation detail rather than
	 * a contract, and "a clean compile is not API verification" applies exactly here.
	 *
	 * So it is done, and then the sweeps MEASURE whether it worked instead of assuming it did. The
	 * report says which of the two mechanisms is carrying the fix.
	 */
	if (ULightweightHierarchicalInstancedStaticMeshComponent* CDO =
			GetMutableDefault<ULightweightHierarchicalInstancedStaticMeshComponent>())
	{
		if (CVarInstanceSwapEnabled.GetValueOnAnyThread() != 0)
		{
			CDO->SetRemoveSwap();
		}
	}

	// Not gated by the diag channel: the stated Arm()-line exception. This is the line that separates
	// "converted nothing because everything was already right" from "never armed".
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] instance remove-swap ARMED - no hook. AbstractInstance keeps its handle table with "
		     "RemoveAtSwap (AbstractInstanceManager.cpp:194-201) but its mesh component became a PLAIN "
		     "ISM at CL480321, whose RemoveInstance shifts down instead (InstancedStaticMesh.cpp:"
		     "3957-3959). The two disagree from the second removal onward, which is why a piece can go "
		     "invisible while staying solid - collision removes the LAST index and is identical under "
		     "both. This calls the engine's own SetRemoveSwap() on those components only, never on the "
		     "collision twin and never through the process-global cvar."));
}

void FFPMInstanceRemoveSwap::Disarm()
{
	/*
	 * ⚠ THE FLAG IS NOT PUT BACK, AND THAT IS THE CORRECT CHOICE — stated because a Disarm that
	 * silently declines to undo something is otherwise indistinguishable from one that forgot.
	 *
	 * There is no `ClearRemoveSwap()` in the engine; the only route would be writing
	 * `bSupportRemoveAtSwap = 0` back, and doing that returns the component to the semantics that
	 * DISAGREE with the live handle table. Any removal after that point corrupts the binding again —
	 * so "undoing" this fix would actively cause the bug it exists to prevent, on a table that has
	 * meanwhile been maintained under swap semantics.
	 *
	 * Disarm therefore stops FUTURE work only: no more sweeps, no more conversions. Zero residue is
	 * unaffected either way — this writes no file, no ini and no cvar hold. It is one bit of runtime
	 * state on a component that ceases to exist when the world unloads.
	 */
	if (ResweepHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ResweepHandle);
		ResweepHandle.Reset();
	}
}

int32 FFPMInstanceRemoveSwap::SweepWorld(UWorld* World, const TCHAR* Moment)
{
	if (World == nullptr) { return 0; }

	const bool bRepair = CVarInstanceSwapEnabled.GetValueOnAnyThread() != 0;

	int32 Seen = 0, Changed = 0, AlreadySet = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<ULightweightHierarchicalInstancedStaticMeshComponent*> Comps;
		It->GetComponents<ULightweightHierarchicalInstancedStaticMeshComponent>(Comps);

		for (ULightweightHierarchicalInstancedStaticMeshComponent* Comp : Comps)
		{
			if (Comp == nullptr) { continue; }
			++Seen;

			/*
			 * `SupportsRemoveSwap()` is the honest question, not the raw bit: the engine's own accessor
			 * ORs in the global cvar (InstancedStaticMesh.cpp:3790), and a derived class is documented
			 * as free to return true regardless (InstancedStaticMeshComponent.h:460). If something else
			 * has already made this component correct, converting it again is a no-op that must not be
			 * counted as work done — the count is this fix's evidence, so inflating it would blind the
			 * CDO experiment below.
			 *
			 * ⚠ THE NAME CAME FROM THE HEADER, THE SECOND TIME. The first attempt called
			 * `SupportsRemoveAtSwap()`, lifted from the local `bUseRemoveAtSwap` in
			 * InstancedStaticMesh.cpp:3801, and did not compile. Worth recording because the engine's
			 * OWN doc comment at :460 is also wrong — it refers to `SetRemoveSwapEnabled()`, which does
			 * not exist; the setter is `SetRemoveSwap()` one line above. Read the declaration, not the
			 * prose beside it and not a variable that merely sounds like it.
			 */
			if (Comp->SupportsRemoveSwap())
			{
				++AlreadySet;
				continue;
			}

			if (bRepair)
			{
				Comp->SetRemoveSwap();
				++Changed;
			}
		}
	}

	++GFPMSwapSweeps;
	GFPMSwapConverted += Changed;
	GFPMSwapAlreadySet = AlreadySet;   // a SNAPSHOT, not a total - it is a property of the world right now
	GFPMSwapLastChanged = Changed;

	if (FPMDiag::IsOn(FPMDiag::EChannel::InstanceSwap))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] instance remove-swap [%s]: %d component(s) seen, %d already correct, %d %s."),
			Moment, Seen, AlreadySet, Changed,
			bRepair ? TEXT("converted") : TEXT("would be converted (FPM.InstanceSwap.Enabled is 0)"));
	}

	/*
	 * ★ ZERO SEEN IS A FINDING, NOT SILENCE — and it is the one result that would invalidate this whole
	 * fix. If no component of this class exists, either AbstractInstance is not in use on this machine,
	 * or the class was renamed by a game update, and in the second case this fix is dead while still
	 * printing a confident armed line. Ungated by the diag channel for that reason.
	 */
	UE_CLOG(Seen == 0, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] instance remove-swap [%s]: NO ULightweightHierarchicalInstancedStaticMeshComponent "
		     "found in this world at all. Either AbstractInstance is unused here, or the class moved and "
		     "this fix is now inert - check before trusting its armed line."), Moment);

	return Changed;
}

void FFPMInstanceRemoveSwap::OnWorldLoad(UWorld* World)
{
	if (World == nullptr) { return; }

	GFPMSwapResweepIndex = 0;
	SweepWorld(World, TEXT("world load"));

	if (ResweepHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ResweepHandle);
		ResweepHandle.Reset();
	}

	TWeakObjectPtr<UWorld> WeakWorld(World);

	ResweepHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this, WeakWorld](float) -> bool
		{
			UWorld* W = WeakWorld.Get();
			if (W == nullptr) { return false; }

			const int32 Changed = SweepWorld(W, TEXT("re-sweep"));

			/*
			 * ★ THE CDO EXPERIMENT'S VERDICT, printed once, the first time a re-sweep runs.
			 *
			 * A re-sweep that changes NOTHING means components created since the world loaded already
			 * carried the flag - i.e. the CDO write propagates and these sweeps are redundant. A
			 * re-sweep that changes something means the CDO route does nothing and the sweeps are
			 * load-bearing. Saying which removes an assumption instead of leaving one in the code.
			 */
			static bool bSaidVerdict = false;
			if (!bSaidVerdict)
			{
				bSaidVerdict = true;
				UE_CLOG(Changed == 0, LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] instance remove-swap: the first re-sweep converted NOTHING, so components "
					     "created after world load already carry the flag - the CDO write propagates and "
					     "these sweeps are belt-and-braces."));
				UE_CLOG(Changed > 0, LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] instance remove-swap: the first re-sweep converted %d component(s), so the "
					     "CDO write does NOT reach components built later. These sweeps are load-bearing, "
					     "and anything created between them is briefly unprotected."), Changed);
			}

			++GFPMSwapResweepIndex;
			if (GFPMSwapResweepIndex >= GFPMSwapResweepCount)
			{
				ResweepHandle.Reset();
				return false;
			}
			return true;
		}),
		GFPMSwapResweepSec[0]);
}

void FFPMInstanceRemoveSwap::ReportNow()
{
	if (GFPMSwapLastChanged < 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] instance remove-swap: NO SWEEP HAS RUN YET. This fix acts at world load, so in "
			     "the main menu that is expected."));
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] instance remove-swap: %d component(s) converted across %d sweep(s); %d were already "
		     "correct at the last sweep; last sweep changed %d. Repair is %s."),
		GFPMSwapConverted, GFPMSwapSweeps, GFPMSwapAlreadySet, GFPMSwapLastChanged,
		CVarInstanceSwapEnabled.GetValueOnGameThread() != 0 ? TEXT("ON") : TEXT("OFF (counting only)"));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   ⚠ This only affects removals made AFTER it armed. Instances already mis-bound by "
		     "earlier removals stay mis-bound until the world reloads, so 'I armed it and the broken "
		     "pieces are still broken' is expected, not a failure."));
}

static FAutoConsoleCommand GFPMInstanceSwapReportCmd(
	TEXT("FPM.InstanceSwap.Report"),
	TEXT("Instance remove-swap: components converted, already-correct, and whether the CDO write reaches "
	     "components built after world load."),
	FConsoleCommandDelegate::CreateStatic(&FFPMInstanceRemoveSwap::ReportNow));
