// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMBootVerify.h"

#include "FicsitsPerformanceManager.h"

#include "Core/FPMCVarWriter.h"
#include "Core/FPMDetectorRegistry.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMGiveTake.h"
#include "Core/FPMLeverRegistry.h"
#include "Core/FPMStageTables.h"
#include "Session/FPMHostTier.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

namespace
{
	/** Same duplicated-emit discipline every other FPM diagnostic command uses: Ar reaches the console
	 *  Ant is looking at, UE_LOG reaches the file an agent reads afterwards. */
	void FPMVerifyEmit(FOutputDevice* Ar, const FString& Line)
	{
		if (Ar != nullptr) { Ar->Log(Line); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *Line);
	}

	enum class EFPMVerifyResult : uint8 { Pass, Fail, Unreachable };

	const TCHAR* FPMVerifyResultName(EFPMVerifyResult Result)
	{
		switch (Result)
		{
		case EFPMVerifyResult::Pass:        return TEXT("PASS");
		case EFPMVerifyResult::Fail:        return TEXT("FAIL");
		case EFPMVerifyResult::Unreachable: return TEXT("UNREACHABLE");
		default:                            return TEXT("UNREACHABLE");
		}
	}

	struct FFPMVerifyRow
	{
		FString Name;
		EFPMVerifyResult Result;
		FString Detail;
	};

	/**
	 * Runs one `IFPMFix`-owned self-test, gated on `FPMFixes::IsArmed`.
	 *
	 * The gate exists because every one of these five subsystems registers its fixture rows inside its
	 * own `Arm()`, not inside `SelfTest()` — an unarmed fix has never populated them, and calling
	 * `SelfTest()` against an empty registry is not a code path this file has verified. UNREACHABLE,
	 * stated with the reason, is the honest answer; it is never counted as a PASS.
	 */
	template <typename SelfTestFn>
	FFPMVerifyRow FPMVerifyRunArmed(const TCHAR* SubsystemName, IFPMFix& Fix, SelfTestFn&& Fn)
	{
		if (!FPMFixes::IsArmed(Fix))
		{
			return FFPMVerifyRow{ SubsystemName, EFPMVerifyResult::Unreachable,
				FString::Printf(TEXT("fix '%s' is not armed this session - its self-test would run "
				                     "against an unregistered fixture set, so it is not called"),
				                Fix.Name()) };
		}
		const bool bPassed = Fn();
		return FFPMVerifyRow{ SubsystemName, bPassed ? EFPMVerifyResult::Pass : EFPMVerifyResult::Fail,
			FString() };
	}

	/**
	 * ★ THE KNOWN-NEGATIVE CONTROL. A real `IConsoleManager` lookup, not a hardcoded `false` literal —
	 * a literal would let a compiler's constant-condition warning fire, or worse, let a later edit
	 * "simplify" the always-false branch away entirely. This asks for a console-variable name no code
	 * anywhere in this project, the engine, or the game registers, so the lookup can only succeed if
	 * this exact string collided with something real (vanishingly unlikely) or `FPM.Verify`'s own
	 * classifier is inverted. Either way `Run()` must not report a clean sheet.
	 */
	bool FPMVerifyKnownNegativeControl()
	{
		return IConsoleManager::Get().FindConsoleVariable(
			TEXT("FPM.Verify.KnownNegative.__DoesNotExist")) != nullptr;
	}
}

void FPMBootVerify::Run(FOutputDevice* Ar)
{
	FPMVerifyEmit(Ar, TEXT("---- FPM.Verify - every subsystem self-test this build owns, one command ----"));

	TArray<FFPMVerifyRow> Rows;

	// 1. FPMCVarWriter - always-on core subsystem, no arm/disarm concept of its own.
	Rows.Add(FFPMVerifyRow{ TEXT("FPMCVarWriter"),
		FPMCVarWriter::Get().SelfTest() ? EFPMVerifyResult::Pass : EFPMVerifyResult::Fail, FString() });

	// 2. FPMFixes - the fix registry's own arm/side-gate invariants. Same reason as above: always-on.
	Rows.Add(FFPMVerifyRow{ TEXT("FPMFixes"),
		FPMFixes::SelfTest() ? EFPMVerifyResult::Pass : EFPMVerifyResult::Fail, FString() });

	// 3-6. The four IFPMFix registries whose fixtures live behind Arm().
	Rows.Add(FPMVerifyRunArmed(TEXT("FFPMDetectorRegistry"), FFPMDetectorRegistry::Get(),
		[] { return FFPMDetectorRegistry::SelfTest(); }));

	Rows.Add(FPMVerifyRunArmed(TEXT("FFPMLeverRegistry"), FFPMLeverRegistry::Get(),
		[] { return FFPMLeverRegistry::Get().SelfTest(); }));

	Rows.Add(FPMVerifyRunArmed(TEXT("FFPMStageTables"), FFPMStageTables::Get(),
		[] { return FFPMStageTables::Get().SelfTest(); }));

	// NeverOnDedicatedServer - on a server this reports UNREACHABLE honestly rather than a FAIL that
	// would misdescribe a side-gate as a defect.
	Rows.Add(FPMVerifyRunArmed(TEXT("FFPMGiveTakeWalk"), FFPMGiveTakeWalk::Get(),
		[] { return FFPMGiveTakeWalk::Get().SelfTest(); }));

	// 7. The host probe self-test.
	Rows.Add(FPMVerifyRunArmed(TEXT("FFPMHostTier"), FFPMHostTier::Get(),
		[] { return FFPMHostTier::SelfTest(nullptr); }));

	for (const FFPMVerifyRow& Row : Rows)
	{
		FPMVerifyEmit(Ar, FString::Printf(TEXT("  %-22s %s%s"), *Row.Name, FPMVerifyResultName(Row.Result),
			Row.Detail.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Row.Detail)));
	}

	int32 Reachable = 0;
	for (const FFPMVerifyRow& Row : Rows)
	{
		if (Row.Result != EFPMVerifyResult::Unreachable) { ++Reachable; }
	}
	FPMVerifyEmit(Ar, FString::Printf(TEXT("  => coverage: %d/%d subsystem(s) reachable this session."),
		Reachable, Rows.Num()));

	// 8. The known-negative control, printed and gated LAST - see the header comment for why a PASS
	// here is a hard error rather than one more row in the table above.
	if (FPMVerifyKnownNegativeControl())
	{
		FPMVerifyEmit(Ar, TEXT("  !! SELF-CHECK FAILURE !! the known-negative control reported PASS. "
		                      "FPM.Verify's own PASS/FAIL classifier is broken - do not trust any line "
		                      "above this one."));
	}
	else
	{
		FPMVerifyEmit(Ar, TEXT("  known-negative control: FAIL (expected) - FPM.Verify's classifier "
		                      "distinguishes pass from fail correctly."));
	}
}

/*
 * `FPM.Verify` - same registration shape `FPM.HostProbe.SelfTest` uses
 * (`FAutoConsoleCommandWithOutputDevice`, `FPMHostTier.cpp`): no `UWorld*` needed, every subsystem
 * self-test this command calls resolves its own state through a singleton `Get()`.
 */
static FAutoConsoleCommandWithOutputDevice GFPMBootVerifyCmd(
	TEXT("FPM.Verify"),
	TEXT("Run every subsystem self-test this build owns (FPMCVarWriter, FPMFixes, FFPMDetectorRegistry, "
	     "FFPMLeverRegistry, FFPMStageTables, FFPMGiveTakeWalk, FFPMHostTier) and print PASS/FAIL/"
	     "UNREACHABLE per subsystem, a coverage ratio, and a known-negative control that proves the "
	     "classifier itself works. Read-only - writes no cvar, arms nothing."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic(
		[](FOutputDevice& Ar) { FPMBootVerify::Run(&Ar); }));
