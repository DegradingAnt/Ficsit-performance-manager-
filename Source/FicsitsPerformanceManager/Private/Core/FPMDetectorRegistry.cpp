// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMDetectorRegistry.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMOverlay.h"
#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

FFPMDetectorRegistry& FFPMDetectorRegistry::Get()
{
	static FFPMDetectorRegistry Instance;
	return Instance;
}

TArray<FFPMDetectorRegistry::FAttribution>& FFPMDetectorRegistry::MutableAttributions()
{
	static TArray<FAttribution> GAttributions;
	return GAttributions;
}

const TArray<FFPMDetectorRegistry::FAttribution>& FFPMDetectorRegistry::GetAttributions()
{
	return MutableAttributions();
}

FString FFPMDetectorRegistry::ExtractModKey(const FString& PathName, bool& OutAmbiguous)
{
	OutAmbiguous = false;

	TArray<FString> Segments;
	// A GetPathName() result looks like "/Game/FactoryGame/Foo.Foo:Bar" or "/PluginName/Bar.Bar" -
	// split on '/' and take the leading segments; the object-name tail after '.' never matters here.
	PathName.ParseIntoArray(Segments, TEXT("/"), true);

	if (Segments.Num() == 0)
	{
		OutAmbiguous = true;
		return TEXT("<unresolved>");
	}

	if (Segments[0] != TEXT("Game"))
	{
		// "/PluginName/..." - the plugin/mod content mount IS the first segment.
		return Segments[0];
	}

	// "/Game/...": the verified vanilla mount is specifically "/Game/FactoryGame/" - see
	// FPMAssetResidency.cpp:28-54 for the same prefix used the same way. A second segment other
	// than FactoryGame is SOME OTHER /Game/-mounted content (a mod can mount there too) and is
	// reported honestly rather than folded into "FactoryGame".
	if (Segments.Num() < 2)
	{
		OutAmbiguous = true;
		return TEXT("Game");
	}
	if (Segments[1] != TEXT("FactoryGame"))
	{
		OutAmbiguous = true; // could be vanilla /Game/ non-FactoryGame content, or a /Game/-mounted mod
	}
	return Segments[1];
}

void FFPMDetectorRegistry::Report(FName DetectorName, const FString& OwnerPath, const FString& Description,
                                   int32 Count)
{
	bool bAmbiguous = false;
	FAttribution Row;
	Row.DetectorName = DetectorName;
	Row.OwnerPath = OwnerPath;
	Row.ModKey = ExtractModKey(OwnerPath, bAmbiguous);
	Row.Description = Description;
	Row.Count = Count;
	MutableAttributions().Add(MoveTemp(Row));
}

void FFPMDetectorRegistry::Clear()
{
	MutableAttributions().Empty();
}

void FFPMDetectorRegistry::ReportNow(FOutputDevice* Ar)
{
	const TArray<FAttribution>& Rows = GetAttributions();

	TMap<FString, int32> RowsPerMod;
	TMap<FString, int64> CountPerMod;
	TSet<FName> DetectorsReported;
	for (const FAttribution& Row : Rows)
	{
		RowsPerMod.FindOrAdd(Row.ModKey) += 1;
		CountPerMod.FindOrAdd(Row.ModKey) += Row.Count;
		DetectorsReported.Add(Row.DetectorName);
	}

	// ★ THE COVERAGE LINE. An empty table means one of two very different things - nothing has
	// run, or five detectors ran and found nothing - and the reader cannot tell them apart without
	// this. RegisteredDetectorNames is the honest denominator: the five names this build ships,
	// stated here rather than derived from FPMFixes::Registered() (which would also count every
	// OTHER fix in the mod and answer a different question).
	static const TArray<FName> RegisteredDetectorNames = {
		TEXT("conveyor-grab-detector"), TEXT("master-material-detector"),
		TEXT("widget-tick-detector"), TEXT("lightweight-census-detector"),
		TEXT("audio-voice-detector"),
	};

	auto Emit = [Ar](const FString& Line)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
		if (Ar) { Ar->Log(Line); }
	};

	Emit(FString::Printf(
		TEXT("[FPM] detect: %d attribution(s) from %d/%d detector(s) that have reported this "
		     "session, across %d mod key(s)."),
		Rows.Num(), DetectorsReported.Num(), RegisteredDetectorNames.Num(), RowsPerMod.Num()));

	// m6164470: every FPM feature reports to the dev overlay. A gauge row (rewritten in place),
	// not an event, because this is the SAME question re-asked whenever FPM.Detect.Report runs.
	FPMOverlay::PostSticky(TEXT("detect"), TEXT("registry"),
		FString::Printf(TEXT("M-DETECT: %d attribution(s), %d/%d detector(s) reported, %d mod(s)"),
			Rows.Num(), DetectorsReported.Num(), RegisteredDetectorNames.Num(), RowsPerMod.Num()));

	if (DetectorsReported.Num() < RegisteredDetectorNames.Num())
	{
		TArray<FString> Silent;
		for (const FName& DetectorName : RegisteredDetectorNames)
		{
			if (!DetectorsReported.Contains(DetectorName))
			{
				Silent.Add(DetectorName.ToString());
			}
		}
		Emit(FString::Printf(
			TEXT("[FPM] detect:   COVERAGE: %s never reported this session - see each one's own "
			     "coverage line for why (world not yet loaded, its trap's own prerequisite absent, "
			     "or it genuinely found nothing to attribute)."),
			*FString::Join(Silent, TEXT(", "))));
	}

	// Per-mod table - the "first per-mod table a real detector run produces" that closes Goal 5
	// (§9.3, m6148872). Sorted by attributed count, worst first, so the table reads as a triage
	// list rather than requiring the reader to sort it themselves.
	CountPerMod.ValueSort([](int64 A, int64 B) { return A > B; });
	for (const TPair<FString, int64>& Entry : CountPerMod)
	{
		Emit(FString::Printf(TEXT("[FPM] detect:   %-28s %3d row(s), %lld total attributed"),
			*Entry.Key, RowsPerMod[Entry.Key], Entry.Value));
	}

	/*
	 * ★ THE ONE WALK IN THIS FILE THAT GROWS WHILE YOU PLAY, SO IT IS THE ONE THAT GETS A CEILING.
	 *
	 * Every other loop above iterates a per-mod aggregate, which is bounded by how many mods are
	 * installed. `Rows` is not: `Report()` at line 66 appends one entry per detector sighting and
	 * nothing trims it, so a long session with a noisy detector makes this listing arbitrarily long.
	 * That is the shape that turned FPM.Hooks.Report into 4,240,126 lines on 2026-08-15.
	 *
	 * ⚠ THE CAP IS ON THE LISTING ONLY. The aggregation loop near the top of this function still walks
	 * EVERY row, so the counts and the per-mod table stay complete and stay correct. Capping the
	 * arithmetic instead of the printing would turn a long report into a wrong one, which is the
	 * strictly worse trade.
	 *
	 * ⚠ AND THE DROPPED ROWS SURVIVE NOWHERE ELSE. `Report()` accumulates without logging, so this
	 * listing is the ONLY place a raw row is ever printed. The ceiling line says so out loud, because a
	 * reader who assumes the log has the rest would be wrong and would not find out.
	 *
	 * 128 is twice the largest listing cap already in this module (FFPMCratesSweep::Report caps its
	 * candidate list at 64). It is a printing budget, not a measurement threshold: nothing about the
	 * answer changes with it, only how much of the backing detail is quoted before the per-mod table
	 * above stops being the thing a reader takes away.
	 */
	if (Ar != nullptr)
	{
		const int32 RawRowCap = 128;
		const int32 Shown = FMath::Min(Rows.Num(), RawRowCap);
		for (int32 Index = 0; Index < Shown; ++Index)
		{
			const FAttribution& Row = Rows[Index];
			Emit(FString::Printf(TEXT("[FPM] detect:     [%s] %s - %s (%d)"),
				*Row.DetectorName.ToString(), *Row.ModKey, *Row.Description, Row.Count));
		}

		const FString Ceiling = FPMCeilingHitLine(Shown, Rows.Num(), TEXT("raw attribution row(s)"),
			TEXT("The counts and the per-mod table above are still built from EVERY row, so they are "
			     "complete. Only this raw listing was cut, and the cut rows are printed nowhere else "
			     "at all: a detector accumulates its rows without logging them. Run FPM.Detect.Clear "
			     "and reproduce the case you care about to get a listing that fits."));
		if (!Ceiling.IsEmpty())
		{
			Emit(Ceiling);
		}
	}
}

bool FFPMDetectorRegistry::SelfTest()
{
	const int32 CountBefore = GetAttributions().Num();

	static const FName ProbeDetector(TEXT("FPM.Detect.SelfTest"));
	const FString ProbePathVanilla = TEXT("/Game/FactoryGame/SelfTestProbe.SelfTestProbe");
	const FString ProbePathMod = TEXT("/FPMSelfTestPlugin/SelfTestProbe.SelfTestProbe");

	Report(ProbeDetector, ProbePathVanilla, TEXT("probe row A"), 3);
	Report(ProbeDetector, ProbePathMod, TEXT("probe row B"), 4);

	const TArray<FAttribution>& Rows = GetAttributions();
	const bool bCountGrew = Rows.Num() == CountBefore + 2;

	bool bAmbiguousVanilla = true; // wrong default on purpose: only a real pass should clear it
	bool bAmbiguousMod = false;
	const FString KeyVanilla = ExtractModKey(ProbePathVanilla, bAmbiguousVanilla);
	const FString KeyMod = ExtractModKey(ProbePathMod, bAmbiguousMod);
	const bool bParserOk = (KeyVanilla == TEXT("FactoryGame")) && !bAmbiguousVanilla
		&& (KeyMod == TEXT("FPMSelfTestPlugin")) && !bAmbiguousMod;

	int64 ProbeTotal = 0;
	int32 ProbeRows = 0;
	for (const FAttribution& Row : Rows)
	{
		if (Row.DetectorName == ProbeDetector)
		{
			ProbeTotal += Row.Count;
			++ProbeRows;
		}
	}
	const bool bArithmeticOk = (ProbeRows == 2) && (ProbeTotal == 7);

	// Remove exactly the two probe rows, leaving whatever real rows existed before this ran -
	// a self-test that leaves fake data in the table a real FPM.Detect.Report would then quote is
	// worse than not testing (this file's own class-comment law).
	MutableAttributions().RemoveAll([](const FAttribution& Row) { return Row.DetectorName == ProbeDetector; });
	const bool bCleanupOk = GetAttributions().Num() == CountBefore;

	const bool bPassed = bCountGrew && bParserOk && bArithmeticOk && bCleanupOk;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: self-test %s - store %s, mod-key parser %s, aggregate arithmetic %s, "
		     "cleanup %s."),
		bPassed ? TEXT("PASSED") : TEXT("FAILED"),
		bCountGrew ? TEXT("ok") : TEXT("FAILED (Report() did not add the probe rows)"),
		bParserOk ? TEXT("ok") : TEXT("FAILED (ExtractModKey misparsed its own probe paths)"),
		bArithmeticOk ? TEXT("ok") : TEXT("FAILED (probe row count/total did not match what was reported)"),
		bCleanupOk ? TEXT("ok") : TEXT("FAILED (probe rows survived cleanup - a real report would quote them)"));

	return bPassed;
}

void FFPMDetectorRegistry::Arm()
{
	const bool bSelfTestPassed = SelfTest();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: M-DETECT registry armed - self-test %s. Report-only sink for the four "
		     "community traps + the audio-voice detector (§9.3); it fixes nothing and hooks "
		     "nothing. FPM.Detect.Report prints the per-mod table."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

namespace
{
	static FAutoConsoleCommandWithOutputDevice GFPMDetectReportCmd(
		TEXT("FPM.Detect.Report"),
		TEXT("Print the M-DETECT per-mod attribution table: every mod key with its row count and "
		     "total attributed cost, plus a coverage line naming which of the five detectors have "
		     "not reported yet this session. Verbose (called from the console, always) also lists "
		     "the raw rows, up to a printed ceiling."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
		{
			FPMReportGate Gate(Ar, TEXT("FPM.Detect.Report"));
			if (Gate.IsRefused())
			{
				return;
			}

			FFPMDetectorRegistry::ReportNow(&Ar);
		}));

	static FAutoConsoleCommand GFPMDetectClearCmd(
		TEXT("FPM.Detect.Clear"),
		TEXT("Empty the M-DETECT attribution table. The log already has every row a detector ever "
		     "reported; this only clears the in-memory aggregate before a fresh detector run."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			FFPMDetectorRegistry::Clear();
			UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] detect: table cleared."));
		}));

	static FAutoConsoleCommand GFPMDetectSelfTestCmd(
		TEXT("FPM.Detect.SelfTest"),
		TEXT("Re-run the registry's own store/parser/arithmetic/cleanup proof on demand, without "
		     "waiting for the next boot."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			FFPMDetectorRegistry::SelfTest();
		}));
}
