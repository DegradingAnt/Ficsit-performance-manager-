// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Vanilla/FPMWireNullGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "FGCircuitConnectionComponent.h"
#include "FGSaveSession.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

/*
 * ★ THE REPAIR IS SEPARABLE FROM THE NAMING, and that split is the point of this cvar.
 *
 * The naming half is pure observation and always safe. The repair half MUTATES WORLD STATE, and this
 * project's standing rule is that state surgery is Ant's call. Default 1 because the alternative that
 * was actually measured is the dedicated server dying inside its autosave with two players connected —
 * but a single cvar returns it to viewer-only without losing the half that identifies the culprit.
 */
static TAutoConsoleVariable<int32> CVarWireGuardRepair(
	TEXT("FPM.WireGuard.Repair"), 1,
	TEXT("Remove NULL entries from power-wire arrays when the sweep finds them. 0 = report only, do not "
	     "touch world state. 1 = compact them out (default). This NEVER destroys a wire, a buildable, a "
	     "player character or an item - a null entry is a hole where an object used to be, not an "
	     "object. Turning this off leaves the naming half running."),
	ECVF_Default);

FFPMWireNullGuard& FFPMWireNullGuard::Get()
{
	static FFPMWireNullGuard Instance;
	return Instance;
}

void FFPMWireNullGuard::Arm()
{
	/*
	 * ⚠ THE WORLD-LOAD SWEEP ALONE DOES NOT PROTECT THE MEASURED SCENARIO. Corrected 2026-08-09, hours
	 * after the first version shipped, when its own commit message was checked against its code.
	 *
	 * That version swept ONLY at OnWorldLoad and at the console command, while calling itself "sweep
	 * null mWires entries before the autosave walks them". It does not: the nulls arrive when a
	 * blueprint carrying unresolvable references is PASTED, which is mid-session, long after the load
	 * sweep has run. The next autosave then walks them and the game thread dies exactly as before. The
	 * guard would have cleaned only damage that was already in the save at login — real, but not the
	 * case that took the server down, and the commit claimed the case it did not cover.
	 *
	 * So the sweep now runs at the save itself. `SaveWorldEndOfFrame` is the frame from the crash
	 * callstack (FGSaveSession.cpp:2165) and is the last point before SaveLevelState builds the object
	 * set, which is where the null is dereferenced. Hooking it means the sweep happens on exactly the
	 * frame that matters and never speculatively.
	 *
	 * Non-virtual private member (FGSaveSession.h:593, under the `private:` at :584), so: plain
	 * SUBSCRIBE_METHOD rather than the _VIRTUAL form, and a Config/AccessTransformers.ini friend to
	 * reach it. It is a save-orchestration function, not a tiny accessor, so funchook has a prologue to
	 * work with — but the install goes through the ledger, which RECORDS a refusal rather than letting
	 * one pass silently.
	 *
	 * Cost is bounded and rare: autosaves are minutes apart, and the sweep is a pointer test per wire
	 * entry. It does not touch the per-frame path.
	 */
	SaveHookHandle = FPM_SUBSCRIBE("wire-null-guard", UFGSaveSession::SaveWorldEndOfFrame,
		[](auto& Scope, UFGSaveSession* Self, UWorld* World, ELevelTick TickType, float DeltaSeconds)
		{
			// Sweep, then FALL THROUGH. The save must still happen -- cancelling it to avoid a crash
			// would trade a crash for silent data loss, which is the worse of the two.
			if (World)
			{
				FFPMWireNullGuard::Get().SweepWorld(World);
			}
		});

	// Not gated by the channel: this is the stated Arm()-line exception in FPMDiag.h, and it is the line
	// that separates "swept and found nothing" from "never swept".
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] wire null guard armed - sweeps at every world save AND on world load, repair %s. "
		     "Guards the autosave path that SIGSEGV'd the dedicated server on 2026-08-09 (null UClass in "
		     "FFastSaveReferenceCollector::HandleObjectReference)."),
		CVarWireGuardRepair.GetValueOnAnyThread() != 0 ? TEXT("ON") : TEXT("OFF (report only)"));
}

void FFPMWireNullGuard::Disarm()
{
	/*
	 * ★ THIS DID NOT EXIST, AND ITS ABSENCE MADE A TOGGLE LIE. Same defect as the Wwise gate: `Arm()`
	 * discarded the subscribe handle and no `Disarm()` was declared, so `FPM.Fix.WireNullGuard 0` and
	 * `FPM.Enabled 0` reported success while the hook stayed installed and kept sweeping every save.
	 *
	 * ⚠ AND THE STRUCTURE GATE PASSED IT. `tools/check_structure.py` grew a `check_disarm_coverage()`
	 * check for exactly this, and on 2026-08-11 it printed `28 fixes, 0 error(s), 0 warning(s)` with
	 * this fix and the Wwise gate both uncovered. The check is being fixed alongside this, because a
	 * gate that certifies the defect it was built to catch is worse than no gate.
	 *
	 * ⚠ WHAT DISARM DOES **NOT** DO HERE, deliberately: it does not undo repairs already made. The sweep
	 * nulls out dangling wire references to stop `FFastSaveReferenceCollector::HandleObjectReference`
	 * dereferencing them; those entries were already unreachable, and "restoring" them would hand the
	 * save path back the pointer that SIGSEGV'd the dedicated server on 2026-08-09. Disarm stops future
	 * sweeps. It is not an undo, and claiming otherwise would be the more dangerous lie.
	 */
	if (SaveHookHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UFGSaveSession::SaveWorldEndOfFrame, SaveHookHandle);
		SaveHookHandle.Reset();
	}
}

void FFPMWireNullGuard::OnWorldLoad(UWorld* World)
{
	/*
	 * A world load is exactly when a save's dangling references become live objects, and it is also the
	 * cheapest moment to walk them: the alternative is sweeping before every autosave, which pays the
	 * cost repeatedly for state that only changes at load. If a null can appear mid-session, the manual
	 * `FPM.WireGuard.Sweep` covers it without making every player pay for the possibility.
	 */
	SweepWorld(World);
}

void FFPMWireNullGuard::SweepWorld(UWorld* World)
{
	if (!World) { return; }

	const bool bRepair = CVarWireGuardRepair.GetValueOnAnyThread() != 0;

	int32 ScannedThisSweep = 0;
	int32 ComponentsHitThisSweep = 0;
	int32 NullsThisSweep = 0;
	int32 RemovedThisSweep = 0;

	// Owners are collected rather than logged inline: one line per offending component would be the
	// noisiest line in the log if the damage is widespread, and the useful artefact is the LIST.
	TArray<FString> OffendingOwners;

	++SweepsRun;

	for (TObjectIterator<UFGCircuitConnectionComponent> It; It; ++It)
	{
		UFGCircuitConnectionComponent* Conn = *It;

		// ⚠ TObjectIterator walks EVERY loaded object of this class, including CDOs, objects belonging
		// to other worlds (PIE, a menu scene) and objects mid-teardown. Sweeping any of those would at
		// best waste time and at worst mutate something that is not this session's world.
		if (!IsValid(Conn)) { continue; }
		if (Conn->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) { continue; }
		if (Conn->GetWorld() != World) { continue; }

		++ScannedThisSweep;

		// Direct member access, via the Config/AccessTransformers.ini friend entry. That is the
		// docs-recommended route over stringly-typed FProperty reflection because it fails at BUILD
		// time if the engine ever renames the field, rather than silently at runtime.
		TArray<TObjectPtr<AFGBuildableWire>>& Wires = Conn->mWires;

		int32 NullsHere = 0;
		for (const TObjectPtr<AFGBuildableWire>& Wire : Wires)
		{
			// The plain null test, NOT IsValid(). A pending-kill wire is a different condition with a
			// different owner and a different fix; widening this guard to cover it would be a guess
			// dressed as thoroughness. The measured crash is a NULL class pointer.
			if (Wire == nullptr) { ++NullsHere; }
		}

		if (NullsHere == 0) { continue; }

		++ComponentsHitThisSweep;
		NullsThisSweep += NullsHere;

		if (FPMDiag::IsOn(FPMDiag::EChannel::WireGuard))
		{
			const AActor* Owner = Conn->GetOwner();
			OffendingOwners.Add(FString::Printf(TEXT("%s (%d null%s of %d)"),
				Owner ? *Owner->GetName() : TEXT("<no owner>"),
				NullsHere, NullsHere == 1 ? TEXT("") : TEXT("s"), Wires.Num()));
		}

		if (bRepair)
		{
			const int32 Before = Wires.Num();
			Wires.RemoveAll([](const TObjectPtr<AFGBuildableWire>& Wire) { return Wire == nullptr; });
			RemovedThisSweep += Before - Wires.Num();

			/*
			 * ⚠ mNumWiresConnected IS READ AND REPORTED, NEVER WRITTEN — and that restraint is
			 * deliberate rather than an oversight.
			 *
			 * It is a `uint8` sibling of the array (FGCircuitConnectionComponent.h:188) and I have NOT
			 * established whether it counts array slots or something narrower, such as wires whose far
			 * end is also connected. Writing `Wires.Num()` into it on that uncertainty would be exactly
			 * the class of guess this project keeps paying for. A mismatch is worth SEEING, so it is
			 * printed; correcting it needs the semantics settled first.
			 */
			if (Conn->mNumWiresConnected != Wires.Num())
			{
				UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::WireGuard), LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] wire guard: %s reports mNumWiresConnected=%d but holds %d wire(s) after "
					     "compaction. NOT corrected - the field's exact meaning is unverified. Report this."),
					Conn->GetOwner() ? *Conn->GetOwner()->GetName() : TEXT("<no owner>"),
					static_cast<int32>(Conn->mNumWiresConnected), Wires.Num());
			}
		}
	}

	ComponentsScanned += ScannedThisSweep;
	ComponentsWithNulls += ComponentsHitThisSweep;
	NullEntriesFound += NullsThisSweep;
	NullEntriesRemoved += RemovedThisSweep;

	// ★ THE DENOMINATOR SHIPS WITH THE COUNT. "0 nulls" and "0 nulls in 4,812 components" are different
	// claims and only the second is evidence - a sweep that found nothing because it scanned nothing is
	// the failure mode this project has hit four times in two days.
	if (NullsThisSweep > 0)
	{
		UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::WireGuard), LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] wire guard: %d NULL wire entr%s across %d of %d circuit connection(s) in '%s'. "
			     "%s. Left unswept, the next autosave walks these and can SIGSEGV the game thread."),
			NullsThisSweep, NullsThisSweep == 1 ? TEXT("y") : TEXT("ies"),
			ComponentsHitThisSweep, ScannedThisSweep, *World->GetName(),
			bRepair
				? *FString::Printf(TEXT("Removed %d"), RemovedThisSweep)
				: TEXT("NOT removed - FPM.WireGuard.Repair is 0"));

		for (const FString& Owner : OffendingOwners)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("[FPM]   %s"), *Owner);
		}
	}
	else
	{
		/*
		 * ⚠ LEVEL 1, NOT 2 — corrected 2026-08-10, and the correction cost a diagnosis to earn.
		 *
		 * This line used to be gated at verbosity 2, so a clean sweep printed NOTHING. On the first live
		 * server run that made two very different states indistinguishable: the server logged 102
		 * vanilla `nullpointers in mWires` while this guard said nothing at all, and there was no way to
		 * tell "swept 4,000 components and found none" from "never swept the server at all". An hour
		 * went into a gap that a single always-printed denominator would have closed instantly.
		 *
		 * That is this file's own opening argument used against it: every count is reported WITH its
		 * denominator precisely so a zero is legible. A silent zero is the one shape that is not.
		 * One line per world save is roughly twelve an hour — cheap, and it is the line that proves the
		 * sweep ran.
		 */
		UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::WireGuard), LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wire guard: clean - 0 null entries in %d circuit connection(s) in '%s' "
			     "(sweep #%d this session)."),
			ScannedThisSweep, *World->GetName(), SweepsRun);
	}
}

void FFPMWireNullGuard::LogSummary(const TCHAR* Reason) const
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] wire guard (%s): %d sweep(s), %d circuit connection(s) scanned | %d component(s) "
		     "held nulls, %d null entr%s found, %d removed | repair %s"),
		Reason, SweepsRun, ComponentsScanned, ComponentsWithNulls, NullEntriesFound,
		NullEntriesFound == 1 ? TEXT("y") : TEXT("ies"), NullEntriesRemoved,
		CVarWireGuardRepair.GetValueOnAnyThread() != 0 ? TEXT("ON") : TEXT("OFF"));
}

/*
 * `FPM.WireGuard.Sweep` — run the sweep now, against the world the local player is in.
 *
 * Output goes through the OUTPUT DEVICE, not only UE_LOG: `Display`-level lines do NOT echo to the
 * in-game console, and a command that looks dead when it actually ran has cost this project whole boot
 * cycles. Ant reads the result in the console; the log keeps the detail.
 */
static FAutoConsoleCommandWithWorldAndArgs GWireGuardSweepCmd(
	TEXT("FPM.WireGuard.Sweep"),
	TEXT("Sweep this world's power-wire arrays for NULL entries now, and report what was found."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World)
		{
			FFPMWireNullGuard::Get().SweepWorld(World);
			FFPMWireNullGuard::Get().LogSummary(TEXT("on request"));
		}));

static FAutoConsoleCommand GWireGuardReportCmd(
	TEXT("FPM.WireGuard.Report"),
	TEXT("Print the wire null guard's running totals without sweeping again."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FFPMWireNullGuard::Get().LogSummary(TEXT("on request"));
	}));
