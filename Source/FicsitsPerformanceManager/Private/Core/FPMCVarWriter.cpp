// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMCVarWriter.h"

#include "FicsitsPerformanceManager.h"

// Clause 6's question now has ONE owner. Both compiled tables and the runtime enumeration live behind
// FPMUserSettingMap; this file no longer keeps a copy of any of them.
#include "Core/FPMSaveSettingsInterceptor.h"   // clause 6's gate: IsHealthy(), fails closed
#include "Core/FPMUserSettingMap.h"

namespace
{
	/*
	 * THE US_*-BACKED TABLES USED TO LIVE HERE. They now live behind `FPMUserSettingMap`, together with
	 * the runtime enumeration, because three callers ask this same question — clause 6 below, the residue
	 * sentinel when it classifies a held cvar, and `FPM.D0` when it audits — and a second copy of a
	 * safety list is a drift bug waiting to happen. This file asks; it no longer answers.
	 */

	/** The one priority FPM writes at. Named once so no call site can quietly choose another. */
	constexpr EConsoleVariableFlags GFPMWriterPriority = ECVF_SetByPluginHighPriority;

	/**
	 * FPM's OWN cvar, existing purely so the self-test never touches a game cvar. Registering a real
	 * variable is what makes the test exercise the actual engine path rather than a mock of it.
	 */
	TAutoConsoleVariable<int32> CVarWriterProbe(
		FPMCVarWriter::SelfTestProbeName(), 0,
		TEXT("FPM's own scratch variable. It steers nothing and is read by nothing; the cvar writer's "
		     "boot self-test writes and releases it so the release path is proven on every boot rather "
		     "than assumed. Safe to ignore."),
		ECVF_Default);
}

FPMCVarWriter& FPMCVarWriter::Get()
{
	static FPMCVarWriter Instance;
	return Instance;
}

const FName& FPMCVarWriter::Tag()
{
	// One tag for the whole mod. UnsetAllConsoleVariablesWithTag keys on it, so it is the OFF switch.
	static const FName Value(TEXT("FPM"));
	return Value;
}

IConsoleVariable* FPMCVarWriter::Vet(FName Owner, const TCHAR* CVarName) const
{
	// CLAUSE 1 — existence. First because it is cheapest AND because it is the one whose silent failure
	// already cost this project months of misattributed measurements.
	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName);
	if (!Var)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] writer REFUSED '%s' for owner '%s': no such console variable in this build. "
			     "A write to a cvar that does not exist is a silent no-op, and every measurement "
			     "attributed to it would be attributed wrongly. Check the spelling against the running "
			     "game, not against a doc."),
			CVarName, *Owner.ToString());
		return nullptr;
	}

	// CLAUSE 5 — banned route. Scalability GROUPS leak through Scalability::SaveState with no gate.
	// Drive the members the group expands to; never the group.
	if (FCString::Strnicmp(CVarName, TEXT("sg."), 3) == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] writer REFUSED '%s' for owner '%s': sg.* scalability groups leak through "
			     "Scalability::SaveState with no gate. Expand the group and drive its member cvars."),
			CVarName, *Owner.ToString());
		return nullptr;
	}

	/*
	 * ★ CLAUSE 6 — the US_*-backed set. LIFTED 2026-08-09, onto the interceptor, by Ant's ruling.
	 *
	 * THE HAZARD IS UNCHANGED AND IT IS REAL: `FGGameUserSettings` serialises every `mUserSettings` entry
	 * on every save with NO dirty gate. Measured on 0.5.7 by `FPM.D0`: 28 cvar-backed settings would be
	 * written by a save right now, and 16 of those 28 sit at exactly their default value. So a value FPM
	 * holds at save time becomes the player's PERMANENT setting and survives uninstall. That single
	 * failure is why the whole zero-residue posture exists.
	 *
	 * WHAT CHANGED. This used to be a BLANKET refusal "until P1.3 ships the SaveSettings interceptor".
	 * P1.3 shipped (c5718bd) and nobody updated the gate, so the refusal outlived its own condition and
	 * became a deadlock: P1.5 Leg B has to hold `t.MaxFPS` to find out whether the settings menu's APPLY
	 * path outranks 0x07, clause 6 refused that hold, and lifting clause 6 was gated on Leg B's answer.
	 *
	 * Ant, 2026-08-09, ruling on exactly that knot:
	 *   *"we'll have to lift it to get the awnser then. the law is more for release than dev env."*
	 *
	 * THE GATE IS NOW THE INTERCEPTOR ITSELF, which is what it was built to be — its header has said
	 * "THE WRITER WILL ASK THIS BEFORE EVERY MAPPED WRITE" since it was written, marked NOT WIRED YET.
	 * This is that wiring. `IsHealthy()` FAILS CLOSED in every uncertain state: before `Arm()` has run, if
	 * the arm-time self-test failed, if any restore has ever failed, and mid-suspension. So a write racing
	 * startup is refused rather than waved through, and a guard that has ever failed never re-opens the
	 * door — it latches, permanently, on purpose.
	 *
	 * ⚠ THIS IS NOT A RELAXATION OF THE ZERO-RESIDUE RULE. It moves the guarantee from "we never write
	 * these" to "we only write these while something is provably standing between the write and the save
	 * file". If that something is not healthy, the write is refused exactly as before.
	 */
	if (IsUserSettingBacked(CVarName) && !FFPMSaveSettingsInterceptor::IsHealthy())
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] writer REFUSED '%s' for owner '%s': it is backed by a US_* game user setting and "
			     "the SaveSettings interceptor is NOT HEALTHY. FGGameUserSettings serialises every entry "
			     "on every save with NO dirty gate, so without the interceptor a value we hold at save "
			     "time would become the player's PERMANENT setting and survive uninstall. "
			     "Remedy: check the boot log for why the guard did not arm, or failed. It latches on "
			     "purpose and never re-arms optimistically."),
			CVarName, *Owner.ToString());
		return nullptr;
	}

	return Var;
}

bool FPMCVarWriter::IsUserSettingBacked(const TCHAR* CVarName)
{
	/*
	 * Kept as a thin forwarder rather than deleted. It is the name the sentinel and `FPM.D0` already
	 * call, and the header's promise — that both reach the SAME judgement the writer's refusal uses —
	 * is now literally true rather than maintained by hand.
	 */
	return FPMUserSettingMap::IsBacked(CVarName);
}

const TCHAR* FPMCVarWriter::SelfTestProbeName()
{
	// The ONE place this literal is hand-typed. §5.2 exists because it used to be typed four times.
	static const TCHAR* const Name = TEXT("FPM.SelfTest.Probe");
	return Name;
}

void FPMCVarWriter::GetHeldCVars(TArray<FString>& Out) const
{
	Out.Reset();
	Out.Reserve(Ledger.Num());
	for (const FHold& H : Ledger) { Out.Add(H.CVar); }
}

void FPMCVarWriter::GetHolds(TArray<FHoldView>& Out) const
{
	/*
	 * A COPY, not a view into the ledger, and that is deliberate. The one caller — the SaveSettings
	 * interceptor — iterates this while calling Release() on each entry, and Release MUTATES the ledger.
	 * Handing out anything that aliases `Ledger` would be iterator invalidation with a cvar write on the
	 * other side of it.
	 *
	 * PriorValue and PriorSetBy are NOT exposed. Restoring is the ENGINE's job through the tagged
	 * history Unset that Release performs; a caller holding its own copy of a prior value would be
	 * hand-rolled capture-and-restore, which is the pattern R33 killed because a captured baseline can
	 * be our own earlier write.
	 */
	Out.Reset();
	Out.Reserve(Ledger.Num());
	for (const FHold& H : Ledger)
	{
		FHoldView V;
		V.CVar   = H.CVar;
		V.Value  = H.Value;
		V.Owner  = H.Owner;
		V.Reason = H.Reason;
		V.Lease  = H.Lease;
		Out.Add(MoveTemp(V));
	}
}

bool FPMCVarWriter::Hold(FName Owner, const TCHAR* CVarName, const TCHAR* Value, const TCHAR* Reason,
                         EFPMLease Lease)
{
	IConsoleVariable* Var = Vet(Owner, CVarName);
	if (!Var) { return false; }

	/*
	 * CLAUSE 4 — ownership, in OBSERVE MODE. Deliberately staged: a day-one hard error here bricks a
	 * session over a bookkeeping mistake, which is the lesson the old mod's S4 taught. It warns for one
	 * release and enforces in the next.
	 *
	 * ⚠ THE SAME OWNER RE-REGISTERING IS NOT A COLLISION. Owner identity is a stable NAME, so the next
	 * world's instance of a world-scoped owner RECLAIMS its own hold. Keying this on an instance pointer
	 * would make every world transition look like a conflict.
	 */
	for (const FHold& Existing : Ledger)
	{
		if (!Existing.CVar.Equals(CVarName, ESearchCase::IgnoreCase)) { continue; }
		if (Existing.Owner == Owner) { break; }   // reclaim, not collision

		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] writer: '%s' is already held by '%s' and '%s' is writing it too. OBSERVE MODE - "
			     "the write proceeds and this becomes a refusal in the next release. Two owners on one "
			     "cvar means the release order decides the final value, which is not a thing to leave to "
			     "chance."),
			CVarName, *Existing.Owner.ToString(), *Owner.ToString());
		break;
	}

	/*
	 * ★ THE PRIOR VALUE IS THE PLAYER'S, NOT OUR LAST ONE — review finding, 2026-08-09.
	 *
	 * Reading the cvar here is only correct the FIRST time we hold it. On a RE-HOLD (a reclaim, or the
	 * governor moving a lever to a new value) the current value is the one WE wrote, so recording it
	 * would make `FPM.Changes` answer "what has this mod changed?" with a value the mod itself set. The
	 * engine's history still releases correctly — this was never a residue bug — but the ledger is the
	 * thing a support dump is read from, and a ledger that misreports the baseline is worse than none.
	 *
	 * So: if we already hold this cvar for this owner, KEEP the prior we captured the first time.
	 */
	FString PriorValue = Var->GetString();
	EConsoleVariableFlags PriorSetBy =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);

	if (const FHold* Previous = Ledger.FindByPredicate([&](const FHold& H)
			{ return H.Owner == Owner && H.CVar.Equals(CVarName, ESearchCase::IgnoreCase); }))
	{
		PriorValue = Previous->PriorValue;
		PriorSetBy = Previous->PriorSetBy;
	}

	// THE WRITE. Tagged, so the engine-native release can find it; at 0x07, so the console still wins.
	Var->Set(Value, GFPMWriterPriority, Tag());

	// Drop any stale entry for this cvar+owner so the ledger cannot grow duplicates across reclaims.
	Ledger.RemoveAll([&](const FHold& H)
		{ return H.Owner == Owner && H.CVar.Equals(CVarName, ESearchCase::IgnoreCase); });

	FHold Entry;
	Entry.CVar       = CVarName;
	Entry.Value      = Value;
	Entry.PriorValue = PriorValue;
	Entry.PriorSetBy = PriorSetBy;
	Entry.Owner      = Owner;
	Entry.Reason     = Reason;
	Entry.Lease      = Lease;
	Ledger.Add(MoveTemp(Entry));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] writer HOLD %s = %s (was %s, %s) owner='%s' lease=%s : %s"),
		CVarName, Value, *PriorValue, GetConsoleVariableSetByName(PriorSetBy), *Owner.ToString(),
		Lease == EFPMLease::Module ? TEXT("module") : TEXT("world"), Reason);
	return true;
}

bool FPMCVarWriter::Release(FName Owner, const TCHAR* CVarName)
{
	const int32 Index = Ledger.IndexOfByPredicate([&](const FHold& H)
		{ return H.Owner == Owner && H.CVar.Equals(CVarName, ESearchCase::IgnoreCase); });

	if (Index == INDEX_NONE)
	{
		// Said out loud rather than returning quietly. "We released it" and "we never held it" are
		// different facts and only one of them means the revert worked.
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] writer: nothing to release - '%s' is not held by '%s'."),
			CVarName, *Owner.ToString());
		return false;
	}

	if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName))
	{
		// ⚠ Unset, NOT a lower-priority Set. 0x07 is an ARRAY-typed priority: a Set would APPEND to the
		// history and leave our entry in it forever, which looks exactly like a working revert.
		Var->Unset(GFPMWriterPriority, Tag());
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] writer RELEASE %s (owner '%s')"), CVarName, *Owner.ToString());
	Ledger.RemoveAt(Index);
	return true;
}

int32 FPMCVarWriter::ReleaseOwner(FName Owner)
{
	TArray<FString> Mine;
	for (const FHold& H : Ledger)
	{
		if (H.Owner == Owner) { Mine.Add(H.CVar); }
	}
	for (const FString& CVar : Mine) { Release(Owner, *CVar); }

	UE_CLOG(Mine.Num() > 0, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] writer: released %d hold(s) for owner '%s'."), Mine.Num(), *Owner.ToString());
	return Mine.Num();
}

void FPMCVarWriter::ReleaseAll(const TCHAR* Reason)
{
	const int32 Count = Ledger.Num();

	// ONE ENGINE CALL. This is the OFF switch's mechanism (IConsoleManager.h:1243) and it is why the
	// switch cannot half-work: there is no per-cvar loop here to get partway through and fail.
	IConsoleManager::Get().UnsetAllConsoleVariablesWithTag(Tag(), GFPMWriterPriority);
	Ledger.Reset();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] writer RELEASE ALL - %d hold(s) dropped via the engine's tagged history (%s). "
		     "Nothing was captured, so nothing can be restored wrongly."),
		Count, Reason);
}

bool FPMCVarWriter::IsHeld(const TCHAR* CVarName) const
{
	return Ledger.ContainsByPredicate([&](const FHold& H)
		{ return H.CVar.Equals(CVarName, ESearchCase::IgnoreCase); });
}

void FPMCVarWriter::LogLedger(FOutputDevice* Ar) const
{
	// Both destinations, always. Ar reaches the console the operator is reading; the log is what
	// survives the session and what a support dump carries. Which one is "the" output depends on who
	// is asking, and on 2026-08-09 both of us needed it at once.
	auto Emit = [Ar](const FString& Line)
	{
		if (Ar) { Ar->Logf(TEXT("%s"), *Line); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *Line);
	};

	Emit(FString::Printf(TEXT("---- cvar ledger: %d hold(s) ----"), Ledger.Num()));

	/*
	 * ★ EVERY ROW IS CHECKED AGAINST THE LIVE VARIABLE. Found 2026-08-09, mid-P1.5, by Ant.
	 *
	 * Until now this printed `H.Value` — what we ASKED to hold — and nothing else. So a hold that had
	 * been BEATEN by a higher-priority writer printed exactly the same line as one that was still in
	 * force. During the 0x07 proof protocol that is the entire question ("did our hold survive the
	 * options-menu apply?"), and the command answering it could not distinguish the two outcomes.
	 * A ledger cannot verify itself: it is a record of intent, and intent is not state.
	 *
	 * Now the live value is read back and compared. A divergence is the LOUDEST thing in the output,
	 * because it means either something outranked us — a real finding worth having — or our release
	 * path failed and the ledger is tracking a hold that no longer exists, which is residue.
	 */
	for (const FHold& H : Ledger)
	{
		IConsoleVariable* Live = IConsoleManager::Get().FindConsoleVariable(*H.CVar);
		const FString LiveValue = Live ? Live->GetString() : FString(TEXT("<GONE>"));
		const FString LiveSetBy = Live
			? FString(GetConsoleVariableSetByName(
				static_cast<EConsoleVariableFlags>(Live->GetFlags() & ECVF_SetByMask)))
			: FString(TEXT("-"));
		const bool bHolding = Live && LiveValue.Equals(H.Value);

		Emit(FString::Printf(
			TEXT("  %-44s = %-12s (was %-12s %s)  owner='%s' %s : %s"),
			*H.CVar, *H.Value, *H.PriorValue, GetConsoleVariableSetByName(H.PriorSetBy),
			*H.Owner.ToString(), H.Lease == EFPMLease::Module ? TEXT("module") : TEXT("world"),
			*H.Reason));
		Emit(FString::Printf(TEXT("      live: %-12s %-16s  %s"), *LiveValue, *LiveSetBy,
			bHolding
				? TEXT("HOLD IN FORCE")
				: TEXT("** OVERRIDDEN - our value is NOT what the game is using **")));
	}

	// An empty ledger is a RESULT, not an absence of output — it is what "FPM is holding nothing" looks
	// like, and it is the state an uninstall has to be able to demonstrate.
	if (Ledger.Num() == 0)
	{
		Emit(TEXT("  (holding nothing - the game is in its own state)"));
	}
}

bool FPMCVarWriter::SelfTest()
{
	static const FName Owner(TEXT("writer-selftest"));
	const TCHAR* const Probe = FPMCVarWriter::SelfTestProbeName();

	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Probe);
	if (!Var)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] writer self-test CANNOT RUN: '%s' is missing. The writer is unverified this boot."),
			Probe);
		return false;
	}

	const FString Before = Var->GetString();
	const EConsoleVariableFlags SetByBefore =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);

	if (!Hold(Owner, Probe, TEXT("4242"), TEXT("boot self-test of the write path")))
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] writer self-test FAILED: the hold was refused on FPM's own probe cvar."));
		return false;
	}

	const FString Held = Var->GetString();
	Release(Owner, Probe);

	const FString After = Var->GetString();
	const EConsoleVariableFlags SetByAfter =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);

	// BOTH halves are checked. A release that restores the VALUE but leaves our SetBy on the variable has
	// not let go — the next writer at a lower priority would still be locked out, and the residue would
	// be invisible to anyone reading only the value.
	const bool bValueOk = (Held == TEXT("4242")) && (After == Before);
	const bool bSetByOk = (SetByAfter == SetByBefore);

	if (bValueOk && bSetByOk)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] writer self-test PASSED: %s %s -> 4242 -> %s, SetBy %s -> %s. Write, hold and "
			     "engine-native release all work on this build."),
			Probe, *Before, *After,
			GetConsoleVariableSetByName(SetByBefore), GetConsoleVariableSetByName(SetByAfter));
		return true;
	}

	UE_LOG(LogFicsitsPerformanceManager, Error,
		TEXT("[FPM] writer self-test FAILED: value %s -> %s -> %s (expected back to '%s'), "
		     "SetBy %s -> %s (expected back to %s). ⚠ THE RELEASE PATH IS NOT WORKING ON THIS BUILD - "
		     "treat every hold as residue until this is understood."),
		*Before, *Held, *After, *Before,
		GetConsoleVariableSetByName(SetByBefore), GetConsoleVariableSetByName(SetByAfter),
		GetConsoleVariableSetByName(SetByBefore));
	return false;
}

/*
 * `FPM.Changes` (§7.15) — what FPM is currently holding, and what it was before.
 *
 * This is the question a player or a support dump actually asks, and until now nothing could answer it:
 * "what has this mod changed on my machine?" The honest answer is a list with prior values beside it.
 */
static FAutoConsoleCommandWithOutputDevice GWriterLedgerCmd(
	TEXT("FPM.Changes"),
	TEXT("Print every console variable FPM is currently holding, with its prior value and prior SetBy."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
		{ FPMCVarWriter::Get().LogLedger(&Ar); }));

static FAutoConsoleCommandWithOutputDevice GWriterOffCmd(
	TEXT("FPM.Off"),
	TEXT("THE OFF SWITCH. Release every console variable FPM holds and leave the game in its own state."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		// Report what it DID, not that it ran. "FPM.Off" printing nothing is indistinguishable from
		// FPM.Off being broken -- the same defect FPM.Changes shipped with.
		FPMCVarWriter& W = FPMCVarWriter::Get();
		TArray<FString> Before;
		W.GetHeldCVars(Before);
		W.ReleaseAll(TEXT("FPM.Off from the console"));
		Ar.Logf(TEXT("FPM.Off: released %d hold(s)%s"), Before.Num(),
			Before.Num() ? *FString::Printf(TEXT(" - %s"), *FString::Join(Before, TEXT(", "))) : TEXT(" (nothing was held)"));
	}));

/*
 * ★ `FPM.Hold` / `FPM.Release` — WHAT MAKES P1.5 RUNNABLE AT ALL.
 *
 * Design R2 §9's P1.5 is "THE 0x07 PROOF BOOT", and it is the last Phase 1 increment: write at 0x07,
 * survive a scalability apply and a vanilla options-menu apply, confirm a console override still WINS,
 * then release and confirm BOTH the value and the SetBy came back. The recorded project law keeps
 * prescribing SetByCode until that boot lands, so this is blocking a law change and not a checkbox.
 *
 * ⚠ IT COULD NOT BE RUN. Discovered 2026-08-09 while writing the protocol out for Ant: nothing in the
 * shipped build makes the writer hold anything on demand. The boot self-test holds and releases inside
 * one frame, and no fix writes a cvar yet. So every instruction of the form "now hold a cvar and change
 * a setting" was unexecutable, and I had handed her exactly that. The gap was in the BUILD, not in the
 * protocol -- worth recording, because the protocol had been reviewed several times and nobody noticed
 * that the mod offered no way to perform step one.
 *
 * CLAUSE 6 NOW GATES ON THE INTERCEPTOR rather than refusing outright (lifted 2026-08-09, Ant's ruling:
 * "we'll have to lift it to get the awnser then. the law is more for release than dev env"). This still
 * routes through Hold(), so a US_*-backed cvar is permitted here only while the SaveSettings interceptor
 * is armed and healthy, and refused with a named reason otherwise.
 *
 * THAT IS WHAT MAKES P1.5 LEG B RUNNABLE. Leg B deliberately targets `t.MaxFPS` -- a US_*-backed cvar --
 * to contest 0x08 SetByGameOverride. Until the lift, the command that had to perform step one of the
 * protocol was refused by the protocol's own safety clause, which is the deadlock Ant ruled on. The
 * boundary is still guarded; it is now guarded by something that can PROVE it is standing there, instead
 * of by a blanket no.
 */
static FAutoConsoleCommandWithArgsAndOutputDevice GWriterHoldCmd(
	TEXT("FPM.Hold"),
	TEXT("Hold a console variable through FPM's writer, at FPM's priority, until released. "
	     "Usage: FPM.Hold <cvar> <value>   (US_*-backed cvars need the SaveSettings interceptor healthy)"),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			if (Args.Num() != 2)
			{
				Ar.Logf(TEXT("usage: FPM.Hold <cvar> <value>"));
				return;
			}
			static const FName ProbeOwner(TEXT("console-probe"));
			const bool bOk = FPMCVarWriter::Get().Hold(ProbeOwner, *Args[0], *Args[1],
				TEXT("held by hand from the console (P1.5 proof protocol)"));
			// Hold() logs its own refusal reason; echo the VERDICT to the console so the operator is
			// not left reading an empty line and guessing whether it took.
			Ar.Logf(TEXT("FPM.Hold %s = %s : %s"), *Args[0], *Args[1],
				bOk ? TEXT("HELD") : TEXT("REFUSED - see the log line above for which clause"));
			if (bOk) { FPMCVarWriter::Get().LogLedger(&Ar); }
		}));

static FAutoConsoleCommandWithArgsAndOutputDevice GWriterReleaseCmd(
	TEXT("FPM.Release"),
	TEXT("Release one hold taken by FPM.Hold. Usage: FPM.Release <cvar>"),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			if (Args.Num() != 1)
			{
				Ar.Logf(TEXT("usage: FPM.Release <cvar>"));
				return;
			}
			static const FName ProbeOwner(TEXT("console-probe"));
			const bool bOk = FPMCVarWriter::Get().Release(ProbeOwner, *Args[0]);
			Ar.Logf(TEXT("FPM.Release %s : %s"), *Args[0],
				bOk ? TEXT("RELEASED") : TEXT("we were not holding it (not an error)"));
		}));
