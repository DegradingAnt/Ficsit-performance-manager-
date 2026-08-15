// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMCrashStamp.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMHookLedger.h"

#include "GenericPlatform/GenericPlatformCrashContext.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"

namespace
{
	/*
	 * The keys, named once. They land in the dump's <GameData> block verbatim, so a human reading a
	 * crash report three months from now sees these strings and nothing else — which is the entire
	 * reason they are prefixed and spelled out rather than terse.
	 */
	const TCHAR* const KeyVersion = TEXT("FPM.Version");
	const TCHAR* const KeySide    = TEXT("FPM.Side");
	const TCHAR* const KeyFixes   = TEXT("FPM.Fixes");
	const TCHAR* const KeyHooks   = TEXT("FPM.Hooks");

	/** Kept so `FPM.CrashStamp` can show what was stamped without needing an actual crash to read it back. */
	TArray<TPair<FString, FString>> GStamped;

	void Stamp(const TCHAR* Key, const FString& Value)
	{
		FGenericCrashContext::SetGameData(Key, Value);
		GStamped.Emplace(FString(Key), Value);
	}
}

void FPMCrashStamp::Register(const FString& VersionName)
{
	GStamped.Reset();

	Stamp(KeyVersion, VersionName);

	/*
	 * WHICH MACHINE THIS WAS. A server dump and a client dump look alike at the top of the callstack
	 * and diverge entirely in what is even possible — the clone sensor and the Wwise gate exist only
	 * on one side each. Recording the side removes a guess from every future dump reading.
	 */
	Stamp(KeySide, IsRunningDedicatedServer() ? TEXT("DedicatedServer") : TEXT("Client"));

	/*
	 * ★ THE ROSTER, WITH EACH FIX'S ORIGIN STATUS. Not just "FPM was loaded" — WHICH fixes were live
	 * and how confident we were about each one's cause. A dump that says a fix marked
	 * "UNKNOWN CAUSE, highest scrutiny" was armed is a very different read from one where every armed
	 * fix is OriginNamed, and that distinction is exactly what OriginStatus() was added to carry.
	 */
	const TArray<IFPMFix*>& Armed = FPMFixes::Armed();
	TArray<FString> FixEntries;
	FixEntries.Reserve(Armed.Num());
	for (const IFPMFix* Fix : Armed)
	{
		if (Fix == nullptr) { continue; }
		FixEntries.Emplace(FString::Printf(TEXT("%s[%s]"), Fix->Name(), LexToString(Fix->OriginStatus())));
	}
	Stamp(KeyFixes, FString::Printf(TEXT("%d: %s"), FixEntries.Num(), *FString::Join(FixEntries, TEXT(","))));

	/*
	 * Hooks are reported as installed-of-attempted, because those two numbers differ exactly when
	 * something went wrong. funchook REFUSES a hook it cannot patch (too-short prologue, IP-relative
	 * offset, no trampoline space) and the ledger records the refusal — so "17 of 18" in a dump names
	 * a real problem that no other artefact would preserve.
	 */
	const TArray<FPMHookRecord>& Records = FPMHookLedger::Records();
	int32 Installed = 0;
	for (const FPMHookRecord& Record : Records)
	{
		if (Record.bInstalled) { ++Installed; }
	}
	Stamp(KeyHooks, FString::Printf(TEXT("%d installed of %d attempted"), Installed, Records.Num()));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] crash stamp registered: %d key(s) into the crash context. A dump from this process "
		     "will name FPM's version, side, armed-fix roster and hook count WITHOUT needing "
		     "FactoryGame.log - which for a server crash is usually gone by the time anyone looks. "
		     "Registered NOW, not at crash time: the previous attempt wrote at crash time via "
		     "OnHandleSystemError and went 0-for-3 on access violations in Shipping."),
		GStamped.Num());
}

void FPMCrashStamp::LogStamped()
{
	if (GStamped.IsEmpty())
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] crash stamp: NOTHING REGISTERED. Either Register() was never called, or it ran "
			     "before any fix armed. Either way a dump from this process would not identify FPM."));
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] crash stamp: %d key(s) live in this process's crash context --"), GStamped.Num());
	for (const TPair<FString, FString>& Entry : GStamped)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM]   %-14s = %s"), *Entry.Key, *Entry.Value);
	}
}

/*
 * `FPM.CrashStamp` — read the stamp back without crashing to do it.
 *
 * The stamp's whole value is that it survives a crash, and the only way to confirm it is present is
 * either to crash on purpose or to ask. The design deleted the scheduled deliberate-crash boot once
 * the 31-dump census supplied the receipt, so asking is what is left — and it costs nothing.
 */
static FAutoConsoleCommandWithOutputDevice GCrashStampCmd(
	TEXT("FPM.CrashStamp"),
	TEXT("Print the keys FPM registered into this process's crash context at startup."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FPMCrashStamp::LogStamped();
	}));
