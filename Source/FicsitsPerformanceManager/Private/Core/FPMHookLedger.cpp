// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMHookLedger.h"

#include "FicsitsPerformanceManager.h"

namespace
{
	TArray<FPMHookRecord> GLedger;
}

FDelegateHandle FPMHookLedger::Install(const TCHAR* Owner, const TCHAR* Target, TFunctionRef<FDelegateHandle()> Installer)
{
	FPMHookRecord Record;
	Record.Owner = Owner;
	Record.Target = Target;
	Record.Order = GLedger.Num();

	if constexpr (WITH_EDITOR)
	{
		// Recorded as REFUSED rather than skipped silently. An absent row and a refused row look the
		// same to a reader otherwise, and "no hooks in the inventory" would then be ambiguous between
		// "editor build" and "arming never ran".
		Record.bInstalled = false;
		GLedger.Add(Record);

		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] hook #%d REFUSED (editor build): %s  owner=%s"), Record.Order, Target, Owner);
		return FDelegateHandle();
	}
	else
	{
		const FDelegateHandle Handle = Installer();

		// bInstalled means SML's macro returned. It does NOT mean funchook succeeded — funchook has
		// six documented ways to refuse a target (too-short prologue, IP-relative offsets it cannot
		// relocate, a back jump into the patched region, no trampoline space, a prologue it cannot
		// disassemble), and four of those have nothing to do with function size. SML reports those on
		// its own log category. Read that, not this column, before concluding a hook is live.
		//
		// Handle.IsValid() is the one signal this function CAN check directly — it is what SML hands
		// back, and an invalid handle here means the install did not take, whatever bInstalled says.
		Record.bInstalled = Handle.IsValid();
		GLedger.Add(Record);

		if (Handle.IsValid())
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] hook #%d armed: %s  owner=%s"), Record.Order, Target, Owner);
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] hook #%d NOT armed: %s  owner=%s  -- install FAILED, invalid handle"),
				Record.Order, Target, Owner);
		}
		return Handle;
	}
}

const TArray<FPMHookRecord>& FPMHookLedger::Records()
{
	return GLedger;
}

void FPMHookLedger::LogInventory()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] ---- hook inventory: %d hook(s) ----"), GLedger.Num());

	for (const FPMHookRecord& Record : GLedger)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   #%d  %-24s  %s%s"),
			Record.Order,
			Record.Owner,
			Record.Target,
			Record.bInstalled ? TEXT("") : TEXT("   (REFUSED)"));
	}

	if (GLedger.Num() == 0)
	{
		// Not a formatting nicety. An empty inventory printed as an empty list reads as a clean bill
		// of health, and a silently-empty log is how this project lost four days once already.
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   NOTHING ARMED. If this is a game build, arming did not run."));
	}
}
