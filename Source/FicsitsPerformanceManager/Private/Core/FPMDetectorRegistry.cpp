// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMDetectorRegistry.h"

#include "FicsitsPerformanceManager.h"
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

	if (Ar != nullptr)
	{
		for (const FAttribution& Row : Rows)
		{
			Emit(FString::Printf(TEXT("[FPM] detect:     [%s] %s - %s (%d)"),
				*Row.DetectorName.ToString(), *Row.ModKey, *Row.Description, Row.Count));
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
		     "every raw row."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
		{
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
