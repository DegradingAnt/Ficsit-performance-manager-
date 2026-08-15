// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMHookLedger.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMFixContract.h"

#include "HAL/IConsoleManager.h"

namespace
{
	TArray<FPMHookRecord> GLedger;

	/**
	 * The REACHED slots, one per call site.
	 *
	 * ⚠ THEY CANNOT LIVE INSIDE FPMHookRecord. GLedger grows, TArray reallocates, and the counting
	 * wrapper holds a raw pointer for the life of the process. A unique pointer per slot gives every
	 * counter a stable address that no later Install can move.
	 */
	TArray<TUniquePtr<FPMHookCounter>> GCounters;

	/** True once LogInventory has printed. See the late-install warning in Install. */
	bool GInventoryPrinted = false;

	/** How many hooks installed AFTER the first inventory was printed. */
	int32 GInstallsAfterInventory = 0;
}

FDelegateHandle FPMHookLedger::Install(const TCHAR* Owner, const TCHAR* Target,
	TFunctionRef<FDelegateHandle(FPMHookCounter*)> Installer)
{
	FPMHookRecord Record;
	Record.Owner = Owner;
	Record.Target = Target;
	Record.Order = GLedger.Num();
	Record.Counter = GCounters.Add_GetRef(MakeUnique<FPMHookCounter>()).Get();

	/*
	 * ★ THE INVENTORY IS PRINTED ONCE, AND A HOOK THAT ARRIVES AFTER IT IS MISSING FROM IT.
	 *
	 * This is not hypothetical. On 2026-08-15 FicsitsPerformanceManager.cpp called LogInventory at
	 * line 359 with about twenty more FPMFixes::Arm calls still to come, so five owners were absent
	 * from the printed list while the list read as complete. A reader cannot see an absence.
	 *
	 * The ledger cannot move that call, so it says so instead. The repair is to call LogInventory
	 * after the LAST Arm.
	 */
	if (GInventoryPrinted)
	{
		++GInstallsAfterInventory;
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] hook #%d installed AFTER the hook inventory was printed: %s  owner=%s. The "
			     "printed inventory is INCOMPLETE. Move FPMHookLedger::LogInventory() below the last "
			     "FPMFixes::Arm() call, or run FPM.Hooks.Report for the full list."),
			Record.Order, Target, Owner);
	}

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
		const FDelegateHandle Handle = Installer(Record.Counter);

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

bool FPMHookLedger::IsCounted(const FPMHookCounter& Counter)
{
	return FPlatformAtomics::AtomicRead(&Counter.bWrapped) != 0;
}

bool FPMHookLedger::AuditCountingWrappers()
{
	bool bOk = true;

	for (const FPMHookRecord& Record : GLedger)
	{
		if (!Record.bInstalled)
		{
			// A refused install never reached the wrapper, and that is correct rather than a defect.
			continue;
		}

		if (Record.Counter == nullptr)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter audit FAILED: hook #%d (%s, owner=%s) has NO counter slot. "
				     "Something called FPMHookLedger::Install outside the FPM_SUBSCRIBE macros."),
				Record.Order, Record.Target, Record.Owner);
			bOk = false;
			continue;
		}

		if (!IsCounted(*Record.Counter))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter audit FAILED: hook #%d (%s, owner=%s) is INSTALLED but its "
				     "handler was never wrapped. Its REACHED count will stay 0 forever, which reads "
				     "exactly like a handler that never runs. Pass the handler through "
				     "FPMHookCount::Wrap in the FPM_SUBSCRIBE variant that installed it."),
				Record.Order, Record.Target, Record.Owner);
			bOk = false;
		}
	}

	return bOk;
}

namespace
{
	/** REACHED for one hook, read atomically. Returns -1 when the slot is missing or unwrapped. */
	int64 FPMLedgerReachedOf(const FPMHookRecord& Record)
	{
		if (Record.Counter == nullptr || !FPMHookLedger::IsCounted(*Record.Counter))
		{
			return -1;
		}
		return FPlatformAtomics::AtomicRead(&Record.Counter->Reached);
	}

	/** How many ledger rows this fix owns, and how many of those actually installed. */
	void FPMLedgerRowsFor(const TCHAR* FixName, int32& OutRows, int32& OutInstalled)
	{
		OutRows = 0;
		OutInstalled = 0;
		for (const FPMHookRecord& Record : FPMHookLedger::Records())
		{
			if (Record.Owner != nullptr && FCString::Strcmp(Record.Owner, FixName) == 0)
			{
				++OutRows;
				if (Record.bInstalled)
				{
					++OutInstalled;
				}
			}
		}
	}

	FString FPMLedgerJoin(const TArray<FString>& Items)
	{
		return Items.Num() > 0 ? FString::Join(Items, TEXT(", ")) : FString(TEXT("none"));
	}
}

void FPMHookLedger::LogInventory()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] ---- hook inventory: %d hook(s), as of this moment ----"), GLedger.Num());

	for (const FPMHookRecord& Record : GLedger)
	{
		const int64 Reached = FPMLedgerReachedOf(Record);

		if (Reached >= 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM]   #%d  %-24s  reached=%lld  %s%s"),
				Record.Order,
				Record.Owner,
				static_cast<long long>(Reached),
				Record.Target,
				Record.bInstalled ? TEXT("") : TEXT("   (REFUSED)"));
		}
		else
		{
			// ⚠ NOT PRINTED AS 0. An uncounted hook and a hook that never fired are different states,
			// and a 0 in the reached column would make them look identical.
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM]   #%d  %-24s  reached=NOT-COUNTED  %s%s"),
				Record.Order,
				Record.Owner,
				Record.Target,
				Record.bInstalled ? TEXT("") : TEXT("   (REFUSED)"));
		}
	}

	if (GLedger.Num() == 0)
	{
		// Not a formatting nicety. An empty inventory printed as an empty list reads as a clean bill
		// of health, and a silently-empty log is how this project lost four days once already.
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   NOTHING ARMED. If this is a game build, arming did not run."));
	}

	/*
	 * ★ COVERAGE. THE NUMBER OF ARMED FIXES THIS INSTRUMENT CANNOT SEE, PRINTED EVERY RUN.
	 *
	 * Several fixes arm without installing a native hook at all. They use engine delegates, tickers or
	 * cvar callbacks, and no REACHED counter can exist for them. Printing only the hook rows would let
	 * a reader count the armed fixes, count the hook rows, and silently assume the difference is
	 * broken. Silence about what a tool cannot see reads as a clean bill of health.
	 *
	 * The set is DERIVED from the ledger, never hand-listed. A hand-listed set goes stale the first
	 * time a fix gains or loses a hook, and then the coverage line is the thing that lies.
	 */
	int32 Counted = 0;
	TArray<FString> NoHookNames;
	TArray<FString> DeclaredNotInstalled;

	const TArray<IFPMFix*>& ArmedFixes = FPMFixes::Armed();
	for (const IFPMFix* Fix : ArmedFixes)
	{
		int32 Rows = 0;
		int32 Installed = 0;
		FPMLedgerRowsFor(Fix->Name(), Rows, Installed);

		if (Rows == 0)
		{
			NoHookNames.Add(Fix->Name());
		}
		else if (Installed == 0)
		{
			DeclaredNotInstalled.Add(Fix->Name());
		}
		else
		{
			++Counted;
		}
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   coverage: %d of %d armed fixes are hook-counted. Another %d install no hook and "
		     "cannot be counted (they use engine delegates, tickers or cvar callbacks): %s"),
		Counted, ArmedFixes.Num(), NoHookNames.Num(), *FPMLedgerJoin(NoHookNames));

	if (DeclaredNotInstalled.Num() > 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   coverage: %d armed fix(es) declared a hook that did NOT install, so they are "
			     "counted by nothing: %s. In an editor build this is the ledger's own refusal. In a "
			     "game build it is an install failure and the fix is doing nothing."),
			DeclaredNotInstalled.Num(), *FPMLedgerJoin(DeclaredNotInstalled));
	}

	// A hook whose owner matches no armed fix is a row nobody can attribute. It means an owner string
	// drifted away from the fix's Name(), and the coverage arithmetic above silently ignores it.
	TArray<FString> OrphanOwners;
	for (const FPMHookRecord& Record : GLedger)
	{
		const bool bMatched = ArmedFixes.ContainsByPredicate([&Record](const IFPMFix* Fix)
		{
			return Record.Owner != nullptr && FCString::Strcmp(Record.Owner, Fix->Name()) == 0;
		});

		if (!bMatched)
		{
			OrphanOwners.AddUnique(Record.Owner != nullptr ? FString(Record.Owner) : TEXT("<null>"));
		}
	}

	if (OrphanOwners.Num() > 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   coverage: %d hook owner(s) match no ARMED fix: %s. Either the fix is not "
			     "armed on this machine, or its FPM_SUBSCRIBE owner string has drifted from its "
			     "Name(). Both make the coverage count above too small."),
			OrphanOwners.Num(), *FPMLedgerJoin(OrphanOwners));
	}

	if (GInstallsAfterInventory > 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   %d hook(s) installed after the FIRST inventory was printed. That first "
			     "printed list was incomplete."), GInstallsAfterInventory);
	}

	AuditCountingWrappers();

	GInventoryPrinted = true;
}

/*
 * `FPM.Hooks.Report` — the inventory and every REACHED count, on demand.
 *
 * ★ THIS IS THE COMMAND THAT ANSWERS ANYTHING, AND THE BOOT LINE IS NOT.
 *
 * At boot every REACHED count is 0, because no hooked function has run yet. That zero is arithmetic,
 * not evidence. The count only becomes a measurement after play, which means it needs a route that a
 * player can run mid-session. Output goes through the OUTPUT DEVICE as well as UE_LOG, because
 * Display lines do not echo to the in-game console and a command that looks dead has cost this
 * project whole boot cycles.
 */
static FAutoConsoleCommandWithOutputDevice GHookLedgerReportCmd(
	TEXT("FPM.Hooks.Report"),
	TEXT("Print every FPM native hook with how many times its handler has run this session, plus the "
	     "coverage line naming the armed fixes that install no hook."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FPMHookLedger::LogInventory();
	}));
