// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMFixContract.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMHookLedger.h"

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
	 * NOT idempotent: of the 50 registered fixes only seven opened with an `if (Handle.IsValid()) { return; }`
	 * when this was counted on 2026-08-15, so calling `Arm()` twice would install a SECOND handler on the
	 * same method. Keeping "is it armed" in the registry means no caller can double-subscribe, and no fix
	 * has to be rewritten to be safe.
	 *
	 * ⚠ ALL THREE ENTRY POINTS MUST HOLD THAT LINE, NOT TWO OF THEM. `RearmAll` and `SetArmed` always
	 * checked; `Arm` did not, until the guard at the top of it. The count above was 28 when it was
	 * written and the fix roster has since grown to 50, which is the other half of why a stale doc is
	 * treated here as a defect rather than as untidiness.
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
	/*
	 * ⚠ ARMING THE SAME FIX TWICE INSTALLS A SECOND HANDLER, AND THIS IS THE GUARD THAT STOPS IT.
	 *
	 * `RearmAll` and `SetArmed` both refuse to arm what is already armed, for the reason spelled out on
	 * `GRegisteredFixes` above: most `Arm()` bodies subscribe unconditionally. `Arm` was the one entry
	 * point WITHOUT that guard, so a duplicated call site double-subscribed and nothing said so.
	 *
	 * ★ IT COSTS REAL WORK, NOT A WRONG NUMBER. SML's `AddHandlerBefore` does `HandlersBefore->Add(...)`
	 * (NativeHookManager.h:342 and 518) - a TArray append, never a replace. A BEFORE handler installed
	 * twice runs twice per event, because `TCallScope::operator()` walks the array one index per call
	 * and recurses while `bForwardCall` holds (NativeHookManager.h:169-180). An AFTER handler installed
	 * twice ALWAYS runs twice: `ApplyCall` walks `*HandlersAfter` with a plain for loop and has no
	 * cancel path at all (NativeHookManager.h:271-273). So a cancelling fix stops its own duplicates,
	 * and a counting or measuring one silently doubles every number it reports.
	 *
	 * ★ THE REGISTRY IS THE RIGHT LIST TO ASK, NOT THE ARMED LIST. A fix that opts out of arming by
	 * default is in the registry and not in the armed list. A second call on that fix must be as quiet
	 * as a second call on an armed one, and asking `GArmedFixes` would let it through to re-log a line
	 * that tells a reader nothing new.
	 *
	 * ⚠ WARNING, NOT SILENCE, AND NOT `AddUnique`. The obvious alternative is to leave the double call
	 * alone and dedupe the array instead. That is the worse repair: by the time the array is touched,
	 * `Fix.Arm()` has already run and the second handler is already live, so a deduped array would
	 * report a correct count over a genuinely doubled hook. It is the exact shape of instrument this
	 * project keeps paying for. The guard prevents the install; the log line names the caller that
	 * must be fixed.
	 */
	if (GRegisteredFixes.Contains(&Fix))
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] refusing to arm '%s' a second time: it is already in the fix registry. Nothing "
			     "was installed, so no hook is doubled - but a duplicate FPMFixes::Arm() call site "
			     "exists and is not supposed to. Find it."), Fix.Name());
		return;
	}

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
		GRegisteredFixes.Add(&Fix);  // Plain Add. See the note on the pair below.
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] fix '%s' registered but NOT armed: it opts out of arming by default, because it "
			     "was measured doing no work while costing a hook. Turn it on with FPM.Fix.<Name> 1 - "
			     "see the fix's header for the measurement."), Fix.Name());
		return;
	}

	/*
	 * ⚠ PLAIN `Add` ON BOTH, AND THAT IS THE DELIBERATE CHOICE RATHER THAN THE LEFTOVER ONE.
	 *
	 * `AddUnique` here would look like the thing that keeps these lists honest and would not be. By the
	 * time either line runs, `Fix.Arm()` above has already installed the handlers, so a deduplicating
	 * Add would hand back a correct-looking count over a genuinely doubled hook - a number that cannot
	 * move no matter how wrong the world gets, which is this project's most expensive recurring defect.
	 *
	 * The guard at the top of this function is the only thing keeping either list unique, and a plain
	 * `Add` is what makes that visible: remove the guard and BOTH lists grow a duplicate entry, so the
	 * inventory reports the fault instead of hiding it. `SelfTest` check 3 asserts exactly that.
	 */
	Fix.Arm();
	GArmedFixes.Add(&Fix);
	GRegisteredFixes.Add(&Fix);

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

static FAutoConsoleCommandWithOutputDevice GFixDumpCmd(
	TEXT("FPM.Diag.Dump"),
	TEXT("Print every armed FPM fix with its side, origin status and diagnostic channel, then every channel level."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FPMFixes::Dump();
	}));

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
	 * A throwaway fix, used only by `SelfTest`. It installs no hook and touches nothing; `Arm()` sets a
	 * flag so the caller can see whether it ran.
	 *
	 * ⚠ THE INVENTORY MUST COME OUT OF EVERY TEST EXACTLY AS IT WENT IN. The census of armed and
	 * registered fixes is the thing a reader trusts, and a probe left behind would add a phantom entry
	 * to it. One test below is careful never to register a probe at all. The other MUST register one,
	 * because refusing a second `Arm()` cannot be proved without a first `Arm()` to refuse - so it
	 * removes the probe again and then checks both counts came back to where they started. The probe is
	 * a stack local, so that removal is not tidiness, it is the difference between a clean list and a
	 * dangling pointer.
	 *
	 * The name is passed in rather than hardcoded so each test's probe names itself in any log line it
	 * causes. A reader who greps a boot log for a warning gets told which check produced it.
	 */
	class FFPMSelfTestProbe final : public IFPMFix
	{
	public:
		explicit FFPMSelfTestProbe(const TCHAR* InName) : ProbeName(InName) {}

		virtual const TCHAR* Name() const override { return ProbeName; }
		virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }
		virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }
		virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Settings; }
		virtual void Arm() override { bArmWasCalled = true; }

		const TCHAR* const ProbeName;
		bool bArmWasCalled = false;
	};

	/**
	 * Stands in for SML's TCallScope in the counting-wrapper proofs below.
	 *
	 * ⚠ ONLY THE SHAPE IS COPIED, AND THAT LIMIT IS STATED RATHER THAN IMPLIED. A real TCallScope
	 * cannot be built outside an installed hook. What the proofs below check is the WRAPPER: that it
	 * forwards a scope by non-const reference, forwards every other argument unchanged, and counts
	 * exactly one call per call. They do not prove that any real hook fires. Only a boot does that.
	 */
	struct FFPMSelfTestScope
	{
		bool bCancelled = false;
		void Cancel() { bCancelled = true; }
	};

	/** What each proof writes into, so every forwarded argument can be checked one by one. */
	struct FFPMSelfTestWrapProbe
	{
		int32 Int = 0;
		float Real = 0.0f;
		FString Text;
		bool bScopeCancelled = false;
		int32 ReturnSeen = 0;
		int32 Calls = 0;
	};
}

namespace
{
	/**
	 * PROVES THE COUNTING WRAPPER, IN THE FOUR HANDLER SHAPES SML GENERATES.
	 *
	 * ★ WHY A CLASSIFIER PROOF AND NOT A HOOK TEST. FPM cannot install a probe hook at boot to prove
	 * the wrapper: installing one costs a funchook detour on a real game function for no player
	 * benefit, and the FPM_SUBSCRIBE ledger would then carry a row that is not a fix. So this proves
	 * the part that CAN fail silently, against a known-positive and a known-negative, the way
	 * FPMCVarWriter::SelfTest proves its write path on its own probe cvar.
	 *
	 * ⚠ FOUR MACRO FAMILIES, THREE DISTINCT HANDLER SIGNATURES. Read against
	 * NativeHookManager.h:233-241 and 254-257, not from memory:
	 *   FPM_SUBSCRIBE and FPM_SUBSCRIBE_VIRTUAL both produce void(ScopeType&, Args...).
	 *   FPM_SUBSCRIBE_AFTER and FPM_SUBSCRIBE_VIRTUAL_AFTER produce void(Args...) when the hooked
	 *   function returns void, and void(const Ret&, Args...) when it does not.
	 * All four families are exercised below. Saying they collapse to three shapes is the honest
	 * version of "four cases pass".
	 *
	 * @return true if every check passed.
	 */
	bool FPMProveCountingWrapper()
	{
		bool bOk = true;

		// KNOWN-NEGATIVE. A slot nothing wrapped must classify as NOT counted. Without this half, the
		// audit could return true for everything and nobody would know.
		FPMHookCounter BareSlot;
		if (FPMHookLedger::IsCounted(BareSlot))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter self-test FAILED: an unwrapped slot classified as counted. "
				     "The audit that finds uncounted hooks now passes everything, so a hook whose "
				     "REACHED count can never move will report as healthy."));
			bOk = false;
		}

		FFPMSelfTestWrapProbe Probe;

		// 1. THE BEFORE SHAPE, as FPM_SUBSCRIBE produces it: void(ScopeType&, Args...).
		FPMHookCounter BeforeSlot;
		auto OnBefore = [&Probe](FFPMSelfTestScope& Scope, int32 A, const FString& B)
		{
			Scope.Cancel();
			Probe.Int = A;
			Probe.Text = B;
			++Probe.Calls;
		};
		auto WrappedBefore = FPMHookCount::Wrap(&BeforeSlot, OnBefore);

		// KNOWN-POSITIVE for the same classifier.
		if (!FPMHookLedger::IsCounted(BeforeSlot))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter self-test FAILED: a wrapped slot classified as NOT counted. "
				     "Every hook will now be reported as uncounted."));
			bOk = false;
		}

		// ⚠ INSTALLING MUST NOT MOVE THE COUNT. An instrument that appears in its own results reports
		// 1 on a hook that never fired, and this project has already shipped that shape once.
		if (FPlatformAtomics::AtomicRead(&BeforeSlot.Reached) != 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter self-test FAILED: wrapping a handler incremented its own "
				     "REACHED count. Every hook will report at least one call it never received."));
			bOk = false;
		}

		FFPMSelfTestScope Scope;
		WrappedBefore(Scope, 7, FString(TEXT("before-arg")));
		WrappedBefore(Scope, 7, FString(TEXT("before-arg")));

		if (!Scope.bCancelled || Probe.Int != 7 || Probe.Text != TEXT("before-arg"))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter self-test FAILED: the BEFORE shape did not forward its "
				     "arguments unchanged. Cancel reached the scope: %s. int arrived as %d, expected "
				     "7. string arrived as '%s', expected 'before-arg'."),
				Scope.bCancelled ? TEXT("yes") : TEXT("NO"), Probe.Int, *Probe.Text);
			bOk = false;
		}

		if (FPlatformAtomics::AtomicRead(&BeforeSlot.Reached) != 2 || Probe.Calls != 2)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter self-test FAILED: two calls produced REACHED=%lld and %d "
				     "handler run(s), expected 2 and 2. The counter does not track calls one for one."),
				static_cast<long long>(FPlatformAtomics::AtomicRead(&BeforeSlot.Reached)), Probe.Calls);
			bOk = false;
		}

		// 2. THE VIRTUAL SHAPE, as FPM_SUBSCRIBE_VIRTUAL produces it. Same signature shape as BEFORE,
		//    different argument types, so a wrapper that only works for one set is caught.
		FPMHookCounter VirtualSlot;
		auto OnVirtual = [&Probe](FFPMSelfTestScope& Scope, float A, int32 B)
		{
			Scope.Cancel();
			Probe.Real = A;
			Probe.Int = B;
		};
		auto WrappedVirtual = FPMHookCount::Wrap(&VirtualSlot, OnVirtual);

		FFPMSelfTestScope VirtualScope;
		WrappedVirtual(VirtualScope, 2.5f, 11);

		if (!VirtualScope.bCancelled || !FMath::IsNearlyEqual(Probe.Real, 2.5f) || Probe.Int != 11
			|| FPlatformAtomics::AtomicRead(&VirtualSlot.Reached) != 1)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter self-test FAILED: the VIRTUAL shape did not forward or did "
				     "not count. float arrived as %f, expected 2.5. int arrived as %d, expected 11. "
				     "REACHED=%lld, expected 1."),
				Probe.Real, Probe.Int,
				static_cast<long long>(FPlatformAtomics::AtomicRead(&VirtualSlot.Reached)));
			bOk = false;
		}

		// 3. THE AFTER SHAPE FOR A VOID RETURN, as FPM_SUBSCRIBE_AFTER produces it on a void function:
		//    no scope and no return value, only the arguments. glass-quality and upscaler-preset both
		//    hook UFGGameUserSettings::ApplyNonResolutionSettings, which returns void.
		FPMHookCounter AfterVoidSlot;
		auto OnAfterVoid = [&Probe](int32 A, const FString& B)
		{
			Probe.Int = A;
			Probe.Text = B;
		};
		auto WrappedAfterVoid = FPMHookCount::Wrap(&AfterVoidSlot, OnAfterVoid);
		WrappedAfterVoid(31, FString(TEXT("after-void")));

		if (Probe.Int != 31 || Probe.Text != TEXT("after-void")
			|| FPlatformAtomics::AtomicRead(&AfterVoidSlot.Reached) != 1)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter self-test FAILED: the AFTER shape for a void return did not "
				     "forward or did not count. int arrived as %d, expected 31. string arrived as "
				     "'%s', expected 'after-void'. REACHED=%lld, expected 1."),
				Probe.Int, *Probe.Text,
				static_cast<long long>(FPlatformAtomics::AtomicRead(&AfterVoidSlot.Reached)));
			bOk = false;
		}

		// 4. THE AFTER SHAPE FOR A NON-VOID RETURN, as FPM_SUBSCRIBE_VIRTUAL_AFTER produces it on a
		//    function that returns a value: void(const Ret&, Args...). clone-sensor hooks
		//    AFGGameMode::FindInactivePlayer this way and reads what the real call returned.
		FPMHookCounter AfterValueSlot;
		auto OnAfterValue = [&Probe](const int32& ReturnValue, int32 A)
		{
			Probe.ReturnSeen = ReturnValue;
			Probe.Int = A;
		};
		auto WrappedAfterValue = FPMHookCount::Wrap(&AfterValueSlot, OnAfterValue);

		const int32 PretendReturn = 99;
		WrappedAfterValue(PretendReturn, 5);

		if (Probe.ReturnSeen != 99 || Probe.Int != 5
			|| FPlatformAtomics::AtomicRead(&AfterValueSlot.Reached) != 1)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] hook-counter self-test FAILED: the AFTER shape for a non-void return did "
				     "not forward or did not count. return value arrived as %d, expected 99. int "
				     "arrived as %d, expected 5. REACHED=%lld, expected 1."),
				Probe.ReturnSeen, Probe.Int,
				static_cast<long long>(FPlatformAtomics::AtomicRead(&AfterValueSlot.Reached)));
			bOk = false;
		}

		// 5. THE LIVE LEDGER. Every hook that actually installed must carry a wrapper, or its REACHED
		//    column is a permanent 0 that reads like a handler which never runs.
		if (!FPMHookLedger::AuditCountingWrappers())
		{
			bOk = false;
		}

		return bOk;
	}
}

bool FPMFixes::SelfTest()
{
	bool bOk = true;

	// 1. An unregistered fix must be refused. This is the side gate's back door.
	FFPMSelfTestProbe Probe(TEXT("selftest-unregistered-probe"));
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

	/*
	 * 3. `Arm()` MUST ARM A NEW FIX, AND MUST REFUSE THE SAME FIX TWICE. BOTH HALVES, IN THAT ORDER.
	 *
	 * ⚠ THE SECOND HALF ALONE PROVES NOTHING. A guard that refuses EVERY fix passes a duplicate-arm
	 * test perfectly, and from the armed count afterwards it is indistinguishable from a guard that
	 * works. So the first half arms a fix the registry has never seen and requires that it DID arm.
	 * Break the guard's predicate in either direction and exactly one of these two halves fails.
	 *
	 * `bArmWasCalled` is the instrument, not the array length, because it is set inside the fix body -
	 * the thing that installs handlers. An array count can be made to look right by an `AddUnique`
	 * while a second handler is already live behind it; this flag cannot.
	 */
	const int32 ArmedBefore = GArmedFixes.Num();
	const int32 RegisteredBefore = GRegisteredFixes.Num();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] fix-registry self-test: arming a throwaway probe and then arming it again. The "
		     "armed line and the refusal WARNING that follow are expected and are this test's own."));

	FFPMSelfTestProbe DuplicateProbe(TEXT("selftest-duplicate-arm-probe"));
	IFPMFix* const ProbePtr = &DuplicateProbe;  // as the arrays store it, so every Contains/Remove is exact.

	// 3a. THE DIRECTION THAT A BROKEN GUARD BREAKS SILENTLY: a genuinely new fix must still arm.
	FPMFixes::Arm(DuplicateProbe);
	if (!DuplicateProbe.bArmWasCalled || !GArmedFixes.Contains(ProbePtr))
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] fix-registry self-test FAILED: Arm() did not arm a fix the registry had never "
			     "seen. Arm() body ran: %s. In the armed list: %s. The duplicate-arm guard is refusing "
			     "everything, which means NO FIX IN THIS BUILD IS INSTALLED."),
			DuplicateProbe.bArmWasCalled ? TEXT("yes") : TEXT("NO"),
			GArmedFixes.Contains(ProbePtr) ? TEXT("yes") : TEXT("NO"));
		bOk = false;
	}

	// 3b. And the second call must not reach the fix body at all.
	DuplicateProbe.bArmWasCalled = false;
	FPMFixes::Arm(DuplicateProbe);
	if (DuplicateProbe.bArmWasCalled || GArmedFixes.Num() != ArmedBefore + 1)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] fix-registry self-test FAILED: arming the same fix twice ran its Arm() body "
			     "again (ran: %s), or the armed list does not hold exactly one probe entry (%d, "
			     "expected %d). Every fix armed from a duplicated call site now has TWO handlers on "
			     "the same method: a BEFORE handler runs twice per event, an AFTER handler always does."),
			DuplicateProbe.bArmWasCalled ? TEXT("YES") : TEXT("no"),
			GArmedFixes.Num(), ArmedBefore + 1);
		bOk = false;
	}

	// 3c. Put the census back, and prove it went back. A stack probe left in either list dangles.
	GArmedFixes.Remove(ProbePtr);
	GRegisteredFixes.Remove(ProbePtr);
	if (GArmedFixes.Num() != ArmedBefore || GRegisteredFixes.Num() != RegisteredBefore)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] fix-registry self-test FAILED: the self-test probe did not come back out of "
			     "the registry. Armed %d, expected %d. Registered %d, expected %d. The inventory now "
			     "counts a fix that does not exist, and one of these lists holds a dangling pointer."),
			GArmedFixes.Num(), ArmedBefore, GRegisteredFixes.Num(), RegisteredBefore);
		bOk = false;
	}

	// 4. The three accessors must agree. Two arrays behind three readers is how an inventory drifts.
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

	/*
	 * 5. THE REACHED COUNTING WRAPPER. Same discipline as the four checks above: it is worth proving
	 * every boot rather than asserting, because a wrapper that drops an argument or that counts at
	 * install time fails SILENTLY. The failure looks like a working hook with a wrong number beside
	 * it, and a wrong number is worse than no number.
	 */
	if (!FPMProveCountingWrapper())
	{
		bOk = false;
	}

	// Counted here rather than taken from Records().Num(), because a REFUSED row is not an installed
	// hook and reporting it as one would overstate what the audit above actually covered.
	int32 InstalledHooks = 0;
	for (const FPMHookRecord& Record : FPMHookLedger::Records())
	{
		if (Record.bInstalled)
		{
			++InstalledHooks;
		}
	}

	UE_CLOG(bOk, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] fix-registry self-test PASSED: unregistered fixes refused, no-op sets report no "
		     "change, and %d armed of %d registered agree across all three accessors. The REACHED "
		     "counting wrapper forwards every argument unchanged and counts one per call in all four "
		     "FPM_SUBSCRIBE families, and all %d installed hook(s) carry it. ⚠ This does NOT prove a "
		     "real arm/disarm CYCLE works, and it does NOT prove any hooked function ever runs - both "
		     "still need a boot. Every REACHED count is 0 until then. Run FPM.Hooks.Report after play."),
		GArmedFixes.Num(), GRegisteredFixes.Num(), InstalledHooks);

	return bOk;
}

void FPMFixes::RearmAll()
{
	/*
	 * ⚠ THIS ARMS ONLY WHAT IS NOT ALREADY ARMED, and that guard is load-bearing rather than defensive.
	 *
	 * Most `Arm()` bodies subscribe unconditionally. Calling one twice installs a second handler on the
	 * same method, and SML appends rather than replaces: `HandlersBefore->Add(...)`
	 * (NativeHookManager.h:342 and 518).
	 *
	 * ⚠ THE OLD VERSION OF THIS NOTE SAID "the cancel runs twice", AND THE BYTES SAY OTHERWISE. Read
	 * `TCallScope::operator()` (NativeHookManager.h:169-180): the chain advances to the next handler
	 * only `if (HandlerPtr == CachePtr && bForwardCall)`, so the FIRST copy to call `Cancel()` stops
	 * every later copy AND the original call. A duplicate cannot cancel twice. What a duplicate DOES
	 * cost is every pass-through call, where no copy cancels and all of them run in turn, and every
	 * AFTER handler unconditionally, because `ApplyCall` walks `*HandlersAfter` with a plain for loop
	 * and has no cancel path at all (NativeHookManager.h:271-273). So the real damage lands on the
	 * counting and measuring fixes, whose numbers double, not on the guards.
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
