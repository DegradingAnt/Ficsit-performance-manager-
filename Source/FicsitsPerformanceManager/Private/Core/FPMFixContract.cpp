// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMFixContract.h"

#include "FicsitsPerformanceManager.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * Armed fixes, in arm order. Raw pointers because every fix is a function-local static owned for
	 * the life of the process — see the Get() accessors. Nothing here manages a lifetime.
	 */
	TArray<IFPMFix*> GArmedFixes;
}

const TCHAR* LexToString(EFPMOriginStatus Status)
{
	/*
	 * The strings say the uncomfortable part out loud on purpose. "choke-point repair" alone reads like a
	 * technique; "CAUSE NOT NAMED" reads like the outstanding work it actually is, and it is the reader of
	 * a boot log — not the author of the fix — who needs to know which they are looking at.
	 */
	switch (Status)
	{
	case EFPMOriginStatus::OriginNamed:      return TEXT("origin named");
	case EFPMOriginStatus::ChokePointRepair: return TEXT("choke-point repair, CAUSE NOT NAMED");
	case EFPMOriginStatus::Guard:            return TEXT("guard, the cause is not ours to own");
	case EFPMOriginStatus::UnknownCause:     return TEXT("UNKNOWN CAUSE, highest scrutiny");
	default:                                 return TEXT("<unclassified>");
	}
}

void FPMFixes::Arm(IFPMFix& Fix)
{
	if (Fix.Side() == EFPMFixSide::NeverOnDedicatedServer && IsRunningDedicatedServer())
	{
		// Logged, not skipped quietly. "The fix is not in the inventory" and "the fix was gated off"
		// are different diagnoses, and a reader of a server log needs to tell them apart.
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] fix '%s' not armed: dedicated server, and it touches client-only systems"), Fix.Name());
		return;
	}

	Fix.Arm();
	GArmedFixes.Add(&Fix);

	// The armed line now carries the CLAIM, per §2.2. A reader skimming a boot log can see at a glance
	// which fixes rest on a named cause and which are holding a symptom down while the cause is still open.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] fix '%s' armed  [%s]  diag=%s"),
		Fix.Name(), LexToString(Fix.OriginStatus()), FPMDiag::NameOf(Fix.Channel()));
}

void FPMFixes::Dump()
{
	/*
	 * `FPM.Diag.Dump` — the whole inventory in one place, per P1.1.
	 *
	 * WHY THIS EXISTS SEPARATELY FROM THE ARMED LINES. The armed lines are scattered through a boot log
	 * among thousands of others and are gone by the time anyone is asking. This is a single block, on
	 * demand, mid-session — and it prints what the armed lines cannot: which fixes are still carrying an
	 * unnamed cause, counted, so the number can be watched going down.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] ---- fix inventory: %d armed ----"), GArmedFixes.Num());

	int32 Unnamed = 0;
	for (const IFPMFix* Fix : GArmedFixes)
	{
		const bool bNamed = Fix->OriginStatus() == EFPMOriginStatus::OriginNamed;
		if (!bNamed) { ++Unnamed; }

		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %-18s %-14s %-34s %s"),
			Fix->Name(),
			Fix->Side() == EFPMFixSide::Any ? TEXT("any-side") : TEXT("client-only"),
			LexToString(Fix->OriginStatus()),
			FPMDiag::NameOf(Fix->Channel()));
	}

	// Not decoration: this is the number §2.2 exists to drive down, and printing it beside the list is what
	// keeps "we should name that cause one day" from becoming permanent.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   %d of %d still carry an UNNAMED cause. Each of those owes an origin-naming diagnostic."),
		Unnamed, GArmedFixes.Num());

	FPMDiag::LogAll();
}

static FAutoConsoleCommand GFixDumpCmd(
	TEXT("FPM.Diag.Dump"),
	TEXT("Print every armed FPM fix with its side, origin status and diagnostic channel, then every channel level."),
	FConsoleCommandDelegate::CreateStatic(&FPMFixes::Dump));

void FPMFixes::NotifyWorldLoad(UWorld* World)
{
	// Arm order, because a later fix may depend on an earlier one having run. Only ARMED fixes are in
	// this array, so a fix the side gate skipped is correctly never notified.
	for (IFPMFix* Fix : GArmedFixes)
	{
		Fix->OnWorldLoad(World);
	}
}

void FPMFixes::DisarmAll()
{
	// Reverse order: a later fix may have installed a hook on top of an earlier one, and unwinding in
	// install order would tear the stack down from the bottom.
	for (int32 Index = GArmedFixes.Num() - 1; Index >= 0; --Index)
	{
		GArmedFixes[Index]->Disarm();
	}
	GArmedFixes.Reset();
}
