// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMFixContract.h"

#include "FicsitsPerformanceManager.h"

namespace
{
	/**
	 * Armed fixes, in arm order. Raw pointers because every fix is a function-local static owned for
	 * the life of the process — see the Get() accessors. Nothing here manages a lifetime.
	 */
	TArray<IFPMFix*> GArmedFixes;
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

	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] fix '%s' armed"), Fix.Name());
}

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
