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

	/**
	 * Everything that PASSED the side gate, whether it is armed right now or not.
	 *
	 * ★ THIS IS WHAT MAKES THE MASTER SWITCH REVERSIBLE. `DisarmAll` empties `GArmedFixes`, so without a
	 * second list an OFF would be permanent for the session — the arm sequence lives in `StartupModule`
	 * and there would be nothing to replay.
	 *
	 * ⚠ AND THE ARMED STATE LIVES HERE RATHER THAN INSIDE EACH FIX, deliberately. Most `Arm()` bodies are
	 * NOT idempotent: of the 28 fixes only a few open with an `if (Handle.IsValid()) return;`, so calling
	 * `Arm()` twice would install a SECOND handler on the same method. Keeping "is it armed" in the
	 * registry means `RearmAll` can never double-subscribe, and no fix has to be rewritten to be safe.
	 *
	 * A fix the side gate refused is in NEITHER list, so a dedicated server can never re-arm a
	 * client-only fix through the back door.
	 */
	TArray<IFPMFix*> GRegisteredFixes;
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

	if (!Fix.DefaultArmed())
	{
		/*
		 * REGISTERED BUT NOT INSTALLED. It still appears in FPM.Fix.List and still has a toggle, so it is
		 * one cvar away — the difference from "not shipped" is visible rather than implied.
		 *
		 * Said out loud for the same reason as the dedicated-server gate above: a reader must be able to
		 * tell "this fix does not exist" from "this fix is deliberately off", and silence cannot.
		 */
		GRegisteredFixes.AddUnique(&Fix);
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] fix '%s' registered but NOT armed: it opts out of arming by default, because it "
			     "was measured doing no work while costing a hook. Turn it on with FPM.Fix.<Name> 1 - "
			     "see the fix's header for the measurement."), Fix.Name());
		return;
	}

	Fix.Arm();
	GArmedFixes.Add(&Fix);
	GRegisteredFixes.AddUnique(&Fix);

	// The armed line now carries the CLAIM, per §2.2. A reader skimming a boot log can see at a glance
	// which fixes rest on a named cause and which are holding a symptom down while the cause is still open.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] fix '%s' armed  [%s]  diag=%s"),
		Fix.Name(), LexToString(Fix.OriginStatus()), FPMDiag::NameOf(Fix.Channel()));
}

const TArray<IFPMFix*>& FPMFixes::Armed()
{
	// One list, two readers: Dump() writes it to the log, the §7.1 support bundle writes it to an
	// FOutputDevice. Neither keeps a copy, so neither can drift from the other.
	return GArmedFixes;
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

	// GRegisteredFixes is deliberately untouched — see its comment. This is what lets RearmAll replay.
}

const TArray<IFPMFix*>& FPMFixes::Registered()
{
	return GRegisteredFixes;
}

bool FPMFixes::IsArmed(const IFPMFix& Fix)
{
	// const_cast only to compare identity — nothing here calls a non-const member.
	return GArmedFixes.Contains(const_cast<IFPMFix*>(&Fix));
}

bool FPMFixes::SetArmed(IFPMFix& Fix, bool bWantArmed)
{
	/*
	 * ⚠ A FIX THE SIDE GATE REFUSED IS NOT IN THE REGISTRY, and must not be armable through here.
	 * Otherwise a per-fix toggle becomes a back door that arms a client-only fix on a dedicated server,
	 * which the gate in `Arm()` exists specifically to prevent.
	 */
	if (!GRegisteredFixes.Contains(&Fix))
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] refusing to arm '%s': it is not registered, which means the side gate refused it "
			     "on this machine. That refusal is not overridable by a toggle."), Fix.Name());
		return false;
	}

	const bool bIsArmed = GArmedFixes.Contains(&Fix);
	if (bIsArmed == bWantArmed)
	{
		return false; // no change — the caller must not log a toggle
	}

	if (bWantArmed)
	{
		Fix.Arm();
		GArmedFixes.Add(&Fix);
	}
	else
	{
		Fix.Disarm();
		GArmedFixes.Remove(&Fix);
	}
	return true;
}

namespace
{
	/**
	 * A fix that is never registered, used only to prove `SetArmed` refuses one.
	 *
	 * ⚠ IT IS DELIBERATELY NOT ARMED AND NOT REGISTERED, so it never appears in the inventory. An
	 * earlier sketch of this test registered a probe fix and cycled it, which would have added a 29th
	 * entry to a list whose whole job is to be an accurate census of what is running.
	 */
	class FFPMUnregisteredProbe final : public IFPMFix
	{
	public:
		virtual const TCHAR* Name() const override { return TEXT("selftest-unregistered-probe"); }
		virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }
		virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }
		virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Settings; }
		virtual void Arm() override { bArmWasCalled = true; }
		bool bArmWasCalled = false;
	};
}

bool FPMFixes::SelfTest()
{
	bool bOk = true;

	// 1. An unregistered fix must be refused. This is the side gate's back door.
	FFPMUnregisteredProbe Probe;
	if (SetArmed(Probe, true) || Probe.bArmWasCalled)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] fix-registry self-test FAILED: SetArmed armed a fix that was never registered. "
			     "A per-fix toggle can now arm something the side gate refused - on a dedicated server "
			     "that means arming a client-only fix."));
		bOk = false;
	}

	// 2. Asking for the state a fix is already in must report NO CHANGE.
	if (GArmedFixes.Num() > 0)
	{
		IFPMFix* First = GArmedFixes[0];
		if (SetArmed(*First, true))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] fix-registry self-test FAILED: SetArmed reported a CHANGE when re-arming '%s', "
				     "which is already armed. Every no-op set will now log a toggle that toggled nothing, "
				     "and Arm() may have run twice - installing a second handler on the same method."),
				First->Name());
			bOk = false;
		}
	}

	// 3. The three accessors must agree. Two arrays behind three readers is how an inventory drifts.
	for (IFPMFix* Fix : GArmedFixes)
	{
		if (!GRegisteredFixes.Contains(Fix) || !IsArmed(*Fix))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] fix-registry self-test FAILED: '%s' is in the ARMED list but disagrees with "
				     "the registry or with IsArmed(). The fix inventory is lying."), Fix->Name());
			bOk = false;
			break;
		}
	}

	UE_CLOG(bOk, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] fix-registry self-test PASSED: unregistered fixes refused, no-op sets report no "
		     "change, and %d armed of %d registered agree across all three accessors. ⚠ This does NOT "
		     "prove a real arm/disarm CYCLE works - that still needs a boot."),
		GArmedFixes.Num(), GRegisteredFixes.Num());

	return bOk;
}

void FPMFixes::RearmAll()
{
	/*
	 * ⚠ THIS ARMS ONLY WHAT IS NOT ALREADY ARMED, and that guard is load-bearing rather than defensive.
	 *
	 * Most `Arm()` bodies subscribe unconditionally. Calling one twice installs a second handler on the
	 * same method, which for a CANCELLING fix means the cancel runs twice and for a counting one means
	 * every number doubles. The registry knowing the state is what makes that impossible.
	 */
	int32 Rearmed = 0;
	for (IFPMFix* Fix : GRegisteredFixes)
	{
		if (GArmedFixes.Contains(Fix)) { continue; }

		Fix->Arm();
		GArmedFixes.Add(Fix);
		++Rearmed;
	}

	/*
	 * ⚠ SAY WHAT RE-ARMING DOES NOT RESTORE. A fix that does its real work in OnWorldLoad — the rain
	 * sweep, the distance-field sampler, the power probe — has just been armed with no world load to
	 * follow, so it sits inert until the next one. Reporting "N re-armed" without that would overstate
	 * what just happened, and this project has paid for exactly that shape of half-true report.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] re-armed %d of %d registered fix(es). ⚠ Fixes whose work happens in OnWorldLoad are "
		     "armed but INERT until the next world load - re-arming does not replay one."),
		Rearmed, GRegisteredFixes.Num());
}
