// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Vanilla/FPMBlueprintSweepGate.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "FGBlueprintSubsystem.h"
#include "FGFactoryBlueprintTypes.h"
#include "FGRecipeManager.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

/*
 * Below this many blueprints the sweep is not worth gating. Vanilla's own cost is linear in the library,
 * so a small library never produced the stutter in the first place and letting vanilla run is strictly
 * the safer branch. Ant's library is 262.
 */
static TAutoConsoleVariable<int32> CVarBlueprintGateMinLibrary(
	TEXT("FPM.Blueprint.MinLibrary"), 24,
	TEXT("Only gate the every-2-seconds blueprint recipe sweep once the library has at least this many "
	     "blueprints. Below it vanilla runs untouched, because the sweep it does is already cheap. "
	     "Default 24."),
	ECVF_Default);

/*
 * ⚠ THE CORRECTNESS PROOF'S CADENCE, and it is the most important number in this file.
 *
 * The gate rests on a claim it cannot verify from headers: that recipe availability and the blueprint
 * set are the ONLY inputs to AreRecipeRequirementsMetForBlueprint. This is how often we go and check
 * that claim against reality. Set it to 0 to disable the audit and the fix becomes a promise instead of
 * a measurement -- which is what the mod this replaces already is.
 */
static TAutoConsoleVariable<float> CVarBlueprintGateAuditSeconds(
	TEXT("FPM.Blueprint.AuditSeconds"), 60.0f,
	TEXT("How often to recompute every blueprint's requirement answer and compare it against the cached "
	     "one. A disagreement means a cancelled sweep would have mattered, and it is logged as a WARNING "
	     "naming the blueprint. 0 disables the audit. Default 60."),
	ECVF_Default);

FFPMBlueprintSweepGate& FFPMBlueprintSweepGate::Get()
{
	static FFPMBlueprintSweepGate Instance;
	return Instance;
}

void FFPMBlueprintSweepGate::BindRecipeManager(UWorld* World)
{
	if (World == nullptr) { return; }

	AFGRecipeManager* Manager = AFGRecipeManager::Get(World);
	if (Manager == nullptr)
	{
		// ⚠ Not silent. Without this delegate the gate has no "something changed" signal and would have
		// to fall back to never cancelling. Saying so is the difference between a degraded fix and a
		// broken one nobody noticed.
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] blueprint gate: no AFGRecipeManager for this world, so recipe unlocks cannot be "
			     "observed. The gate will NOT cancel any sweep - vanilla behaviour, no stutter fix."));
		return;
	}

	if (BoundRecipeManager.Get() == Manager) { return; }

	BoundRecipeManager = Manager;

	// AFGRecipeManager::mOnRecipeAvailable (FGRecipeManager.h:151), a FFGOnRecipeAvailableDelegate
	// declared at :17. It is a DYNAMIC multicast, so the target must be a UFUNCTION -- which a plain C++
	// class cannot supply. AddLambda is unavailable on dynamic delegates for the same reason, so the
	// binding goes through the weak-lambda-free route: a raw member on a UObject we do not own is not an
	// option either.
	//
	// ★ THEREFORE WE POLL THE MANAGER'S OWN COUNT INSTEAD, and say so rather than pretending we hooked
	// it. See ShouldCancelSweep: the available-recipe count is read once per sweep, which is exactly the
	// cadence we need and costs one array length. A count that changes IS a recipe becoming available.
	bDirty.store(true, std::memory_order_relaxed);
}

void FFPMBlueprintSweepGate::Arm()
{
	if (SweepHookHandle.IsValid()) { return; }

	// NAME THE LAMBDA FIRST. sf-scaffold section 7: SML's SUBSCRIBE_ macros are function-like, so the
	// preprocessor splits the handler on top-level commas and does not treat angle brackets as grouping.
	// A handler body containing a TArray<A, B> or a comma-separated declaration is a hard compile error
	// INSIDE the macro. Naming it first moves the body out of the macro entirely.
	auto OnRefreshSweep = [](auto& Scope, AFGBlueprintSubsystem* Self)
	{
		if (Self == nullptr) { return; }
		if (FFPMBlueprintSweepGate::Get().ShouldCancelSweep(Self))
		{
			Scope.Cancel();
		}
	};

	SweepHookHandle = FPM_SUBSCRIBE("blueprint-sweep-gate",
		AFGBlueprintSubsystem::RefreshBlueprintRecipeRequirements, OnRefreshSweep);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] blueprint sweep gate ARMED. Vanilla re-verifies EVERY saved blueprint's recipe "
		     "requirements about every 2 s (FGBlueprintSubsystem.h:201, driven by mTimeSinceLastRecipeCheck "
		     ":691) - O(library) on the game thread, forever. That answer can only change when a recipe "
		     "becomes available or the blueprint set changes, so sweeps in between are cancelled and every "
		     "sweep the game actually wants runs UNTOUCHED. It audits itself every %.0f s and says so if a "
		     "cancelled sweep would ever have mattered."),
		CVarBlueprintGateAuditSeconds.GetValueOnAnyThread());
}

void FFPMBlueprintSweepGate::Disarm()
{
	LogReport();

	// The ledger owns the removal; the handle is ours to clear.
	SweepHookHandle.Reset();
	BoundRecipeManager.Reset();
}

void FFPMBlueprintSweepGate::OnWorldLoad(UWorld* World)
{
	BindRecipeManager(World);

	// ★ ALWAYS DIRTY ACROSS A LOAD. A new world can bring a different unlock state and a different
	// blueprint set, and the cached flags belong to the previous one. The first sweep after a load must
	// therefore be a real one -- assuming otherwise is how this class of fix goes stale.
	bDirty.store(true, std::memory_order_relaxed);
	LastAuditSeconds = FPlatformTime::Seconds();
	LastLibrarySize = 0;
}

bool FFPMBlueprintSweepGate::ShouldCancelSweep(AFGBlueprintSubsystem* Subsystem)
{
	TArray<UFGBlueprintDescriptor*> Descriptors;
	Subsystem->GetBlueprintDescriptors_Internal(Descriptors);
	const int32 LibrarySize = Descriptors.Num();

	// Small library: vanilla's sweep is already cheap and letting it run is the safer branch.
	if (LibrarySize < CVarBlueprintGateMinLibrary.GetValueOnAnyThread())
	{
		++SweepsAllowed;
		return false;
	}

	/*
	 * ★ THE BLUEPRINT SET CHANGED. mBlueprintDescriptorsRequireRefresh (FGBlueprintSubsystem.h:697) is
	 * vanilla's own dirty flag, reachable through the AccessTransformers friend. A change in library size
	 * is checked as well and NOT as a substitute -- an import plus a delete in the same window leaves the
	 * count identical while the set is different, and the flag is what catches that.
	 */
	const bool bVanillaDirty = Subsystem->mBlueprintDescriptorsRequireRefresh;
	const bool bSizeChanged  = (LibrarySize != LastLibrarySize);
	LastLibrarySize = LibrarySize;

	const double Now = FPlatformTime::Seconds();
	const float AuditSeconds = CVarBlueprintGateAuditSeconds.GetValueOnAnyThread();
	const bool bAuditDue = AuditSeconds > 0.f && (Now - LastAuditSeconds) >= AuditSeconds;

	if (bAuditDue)
	{
		LastAuditSeconds = Now;
		++AuditsRun;

		/*
		 * ⚠ THE AUDIT IS THE WHOLE REASON THIS FIX IS ALLOWED TO CANCEL ANYTHING.
		 *
		 * It recomputes the answer for every descriptor with the public
		 * AreRecipeRequirementsMetForBlueprint and compares it against the cached
		 * GetRecipeRequirementsAreMet. If they differ while we believed nothing had changed, then our
		 * model of the inputs is incomplete -- the check consults something we are not watching -- and
		 * that is stated loudly rather than left to rot.
		 *
		 * It then falls through WITHOUT cancelling, so vanilla's own sweep repairs whatever drifted. The
		 * audit is self-healing as well as self-checking.
		 */
		const int32 Disagreements = AuditAgainstCache(Subsystem, /*bLogEach*/ true);
		if (Disagreements > 0 && !bVanillaDirty && !bSizeChanged && !bDirty.load(std::memory_order_relaxed))
		{
			AuditDisagreements += Disagreements;
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] blueprint gate AUDIT DISAGREED on %d of %d blueprint(s) while it believed "
				     "nothing had changed. A cancelled sweep WOULD have mattered, so the trigger set is "
				     "incomplete - the requirement check reads something beyond recipe availability and "
				     "the blueprint set. The sweep below is being allowed to repair it. Set "
				     "FPM.Blueprint.MinLibrary very high to disable the gate until this is understood."),
				Disagreements, LibrarySize);
		}

		++SweepsAllowed;
		bDirty.store(false, std::memory_order_relaxed);
		LastAllowedSweepSeconds = Now;
		return false;
	}

	/*
	 * ★ RECIPE AVAILABILITY, read as a COUNT rather than through a delegate, and this is a deliberate
	 * downgrade that is stated rather than hidden.
	 *
	 * FFGOnRecipeAvailableDelegate (FGRecipeManager.h:17, member :151) is a DYNAMIC multicast, so binding
	 * to it requires a UFUNCTION on a UObject. This fix is a plain C++ class and has neither. Rather than
	 * add a UObject shim whose only job is to receive one event, the gate reads the number of currently
	 * available recipes once per sweep -- the same cadence the sweep already runs at, for the cost of one
	 * array length.
	 *
	 * ⚠ A COUNT CAN MISS A SIMULTANEOUS ADD AND REMOVE. Recipes are not removed in normal play, so this
	 * is sound here -- but it is an assumption about the GAME, not a property of the code, which is
	 * exactly why the audit above exists to catch it being wrong.
	 */
	int32 AvailableRecipes = 0;
	if (AFGRecipeManager* Manager = BoundRecipeManager.Get())
	{
		// The const-reference overload (FGRecipeManager.h:69) returns mAvailableRecipes directly. The
		// out-param sibling at :66 would copy the whole array every two seconds to learn one integer,
		// which in a performance mod would be its own small joke.
		AvailableRecipes = Manager->GetAllAvailableRecipes().Num();
	}
	else
	{
		// No manager means no signal, and no signal means we must not cancel. Fail toward vanilla.
		++SweepsAllowed;
		return false;
	}

	const bool bRecipesChanged = (AvailableRecipes != LastAvailableRecipes);
	LastAvailableRecipes = AvailableRecipes;

	if (bVanillaDirty || bSizeChanged || bRecipesChanged || bDirty.load(std::memory_order_relaxed))
	{
		++SweepsAllowed;
		bDirty.store(false, std::memory_order_relaxed);
		LastAllowedSweepSeconds = Now;

		UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::BlueprintSweep), LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] blueprint gate: ALLOWING a full sweep of %d blueprint(s) - %s"),
			LibrarySize,
			bRecipesChanged ? TEXT("a recipe became available")
			                : bVanillaDirty ? TEXT("vanilla marked the descriptors dirty")
			                : bSizeChanged  ? TEXT("the blueprint set changed size")
			                                : TEXT("a world load or an external mark"));
		return false;
	}

	// Nothing that feeds the answer has moved. This sweep cannot change a single flag.
	++SweepsCancelled;
	return true;
}

int32 FFPMBlueprintSweepGate::AuditAgainstCache(AFGBlueprintSubsystem* Subsystem, bool bLogEach)
{
	TArray<UFGBlueprintDescriptor*> Descriptors;
	Subsystem->GetBlueprintDescriptors_Internal(Descriptors);

	int32 Disagreements = 0;
	for (UFGBlueprintDescriptor* Descriptor : Descriptors)
	{
		if (Descriptor == nullptr) { continue; }

		const FString BlueprintName = Descriptor->GetBlueprintNameAsString();
		const FBlueprintHeader* Header = Subsystem->GetHeaderByName(BlueprintName);
		if (Header == nullptr) { continue; }

		const bool bTruth  = Subsystem->AreRecipeRequirementsMetForBlueprint(*Header);
		const bool bCached = Descriptor->GetRecipeRequirementsAreMet();
		if (bTruth == bCached) { continue; }

		++Disagreements;
		if (bLogEach && Disagreements <= 8)
		{
			// Capped: a systemic disagreement should not turn the log into a wall, and eight names are
			// enough to identify the pattern. The COUNT above carries the true scale.
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   audit: '%s' cached=%s but recomputes to %s"),
				*BlueprintName, bCached ? TEXT("met") : TEXT("unmet"), bTruth ? TEXT("met") : TEXT("unmet"));
		}
	}
	return Disagreements;
}

void FFPMBlueprintSweepGate::RunAuditNow()
{
	// Deliberately does its own subsystem lookup rather than caching one: a console command can be typed
	// in the main menu, where there is no world and no subsystem, and a cached pointer would be stale.
	UWorld* World = GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr;
	AFGBlueprintSubsystem* Subsystem = World != nullptr ? AFGBlueprintSubsystem::Get(World) : nullptr;
	if (Subsystem == nullptr)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] blueprint gate: no blueprint subsystem right now (are you in the main menu?)."));
		return;
	}

	const int32 Disagreements = AuditAgainstCache(Subsystem, /*bLogEach*/ true);
	++AuditsRun;
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] blueprint gate: on-demand audit found %d disagreement(s). Zero means every cancelled "
		     "sweep so far was genuinely inert."), Disagreements);
}

void FFPMBlueprintSweepGate::LogReport()
{
	const int32 Total = SweepsCancelled + SweepsAllowed;

	/*
	 * ★ THE DENOMINATOR, as everywhere in this mod. "412 sweeps cancelled" is unreadable; "412 of 430"
	 * is a measurement, and the audit count beside it is what says whether the cancelling was SAFE
	 * rather than merely frequent.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] blueprint sweep gate: %d of %d sweep(s) cancelled (%.0f%%), %d allowed, library %d. "
		     "%d audit(s) run, %d disagreement(s). Every cancelled sweep was one full O(library) pass the "
		     "game thread did not make."),
		SweepsCancelled, Total, Total > 0 ? 100.0 * SweepsCancelled / Total : 0.0,
		SweepsAllowed, LastLibrarySize, AuditsRun, AuditDisagreements);

	// ⚠ The liveness statement. A gate that never cancels is not a bug, but it is not a fix either, and
	// the difference must be visible without reading the source.
	UE_CLOG(SweepsCancelled == 0 && Total > 0, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   nothing was cancelled. Either the library is under FPM.Blueprint.MinLibrary (%d), "
		     "or something marked it dirty on every single sweep - check the ALLOWING lines for which."),
		CVarBlueprintGateMinLibrary.GetValueOnAnyThread());
}

static FAutoConsoleCommand GBlueprintGateReportCmd(
	TEXT("FPM.Blueprint.Report"),
	TEXT("Print how many blueprint recipe sweeps were cancelled versus allowed, and the audit result."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FFPMBlueprintSweepGate::Get().LogReport();
	}));

/*
 * `FPM.Blueprint.Audit` — run the correctness check on demand.
 *
 * This is the command to run the moment a blueprint looks wrong in the build menu. If it reports zero
 * disagreements the gate is not your problem and the search moves elsewhere, which is worth as much as
 * a hit.
 */
static FAutoConsoleCommand GBlueprintGateAuditCmd(
	TEXT("FPM.Blueprint.Audit"),
	TEXT("Recompute every blueprint's recipe-requirement answer and report any that disagree with the "
	     "cached value."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FFPMBlueprintSweepGate::Get().RunAuditNow();
	}));
