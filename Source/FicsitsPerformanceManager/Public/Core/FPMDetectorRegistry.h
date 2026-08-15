// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ SLICE 4 - M-DETECT, THE COST-ATTRIBUTION CHANNEL. Design §9.3 / §14 Slice 4: "M-DETECT
 * detectors (§9.3, the four traps + the registry + audio voices + UObject watermark §7.3)".
 *
 * ★ WHAT M-DETECT IS, STATED AS A REFUSAL FIRST (§9.3): one reporting pipeline. Detectors
 * OBSERVE, attribute a measured cost to a NAMED cause (a mod, an asset, a setting), and report
 * through FPMDiag/overlay/FPM.Detect.Report - NEVER FIX, NEVER HOOK THE CULPRIT. Hooking another
 * mod's code makes FPM own every interaction bug in it forever; "guard the bug, keep the feature"
 * only works against VANILLA surfaces (§5.10). The narrow M-CARRY exception (§9.13) is a
 * different mechanism with its own four-clause admission test and does not live here.
 *
 * ★ THIS FILE IS THE SINK, NOT A DETECTOR. Each of the four community traps (§9.3) is its own
 * `IFPMFix` in `Fixes/Interop/` - `FPMConveyorGrabDetector`, `FPMMasterMaterialDetector`,
 * `FPMWidgetTickDetector`, `FPMLightweightCensusDetector` - plus `FPMAudioVoiceDetector` for the
 * Wwise voice-starvation trap. All five call `Report()` here instead of keeping their own table,
 * for the same reason `FPMHookLedger` is the one place every hook registers: goal 5 (§9.3,
 * m6148872 - "fix or improve other mods' features") rides THIS aggregation, keyed by mod
 * reference, and a second table anywhere would be a second table that could drift from this one.
 *
 * ★ REPORT-ONLY MEANS THE ONLY WRITE THIS CLASS EVER PERFORMS IS APPENDING TO ITS OWN IN-MEMORY
 * TABLE. No cvar, no ini, no hook - a detector that quietly started fixing what it was built to
 * name would be the exact drift Ruling Q1 (`EFPMOriginStatus`) exists to catch, applied to a
 * whole channel instead of one fix.
 *
 * ★ THE MOD KEY IS A HEURISTIC AND SAYS SO. `ExtractModKey` reads the first meaningful path
 * segment off a `GetPathName()` - `/Game/FactoryGame/...` collapses to `FactoryGame` (the
 * verified vanilla mount, see `FPMAssetResidency.cpp:28-54` for the same prefix used the same
 * way), and `/SomePluginName/...` collapses to `SomePluginName`. It cannot tell a mod's own
 * `/Game/`-mounted content apart from vanilla's - SML mods CAN mount under `/Game/` - so a path
 * under `/Game/` that is not `/Game/FactoryGame/` is labelled by its second segment and flagged
 * `bPathAmbiguous`, never silently folded into "FactoryGame". Callers decide what to do with that;
 * this file only refuses to overclaim precision it does not have.
 *
 * ★ DONE (§14 Slice 4): "each detector names a real culprit on her save or prints its coverage
 * saying why it cannot." That is a property of the FIVE DETECTORS, not of this registry - this
 * file's own liveness proof is narrower and structural: prove the table stores what is reported
 * and aggregates it correctly, the same way `FPM.Wrist.SelfTest` round-trips a probe entry
 * through the wrist registry instead of asserting the store works.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMDetectorRegistry final : public IFPMFix
{
public:
	static FFPMDetectorRegistry& Get();

	virtual const TCHAR* Name() const override { return TEXT("detector-registry"); }

	/** Any - the CONTAINER is side-agnostic; each reporting detector states its own Side(). */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** UnknownCause - same choice as FPMLeverRegistry and FPMHookLedger: nothing here is being
	 *  fixed, the whole value is structural aggregation. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Detect; }

	/** Runs SelfTest() and prints the armed line. Registration itself needs no world. */
	virtual void Arm() override;

	/** One attribution row: a detector naming a cost against a class/asset path. */
	struct FAttribution
	{
		FName DetectorName;
		/** `GetPathName()` of the class/asset the cost is attributed to - the literal string
		 *  reported, per trap 1's own spec ("path names the mod"): naming by full path rather
		 *  than a parsed-out short name that could be wrong. */
		FString OwnerPath;
		/** Derived from OwnerPath by ExtractModKey - the aggregation key. */
		FString ModKey;
		/** Free text: what was found, e.g. "Blueprint Factory_Tick override, 340 live instances". */
		FString Description;
		/** The count the detector is attributing - instances, classes, whatever its own unit is.
		 *  Not comparable ACROSS detectors; each row's Description states its own unit. */
		int32 Count = 0;
	};

	/**
	 * Append one attribution. Safe to call every detector run - rows are NOT deduplicated across
	 * calls, because a detector re-running (a console command fired twice) is a fact worth keeping
	 * rather than a duplicate worth hiding; `FPM.Detect.Clear` exists for a fresh table.
	 *
	 * ★ THIS IS THE ONLY WRITE PATH. A caller that wants to update the aggregate calls this again
	 * with a fresh count rather than mutating a row in place - there is no mutation API, on
	 * purpose, so nothing can quietly rewrite a finding after the fact.
	 */
	static void Report(FName DetectorName, const FString& OwnerPath, const FString& Description,
	                    int32 Count);

	/** Empty the table. `FPM.Detect.Clear`. Does not touch the log - every `Report()` call already
	 *  reached FactoryGame.log via the detector's own diagnostic channel; this only clears the
	 *  in-memory aggregate a re-run would otherwise pile onto. */
	static void Clear();

	/** `FPM.Detect.Report` - the per-mod table Goal 5 (§9.3) rides: every ModKey with its row
	 *  count and total attributed Count, then the raw rows beneath verbose. Prints its own
	 *  coverage line - how many detectors have reported at least once this session, out of how
	 *  many are registered - so an empty table reads as "nothing has run yet" rather than "found
	 *  nothing", which are different facts. */
	static void ReportNow(class FOutputDevice* Ar = nullptr);

	/** Derives the aggregation key from a `GetPathName()`-shaped string. Exposed so every detector
	 *  uses the SAME derivation instead of five slightly different parses. See the class comment's
	 *  ambiguity note. */
	static FString ExtractModKey(const FString& PathName, bool& OutAmbiguous);

	/**
	 * ★ THE LIVENESS PROOF, RUN AT ARM() - same discipline as `FPMCVarWriter::SelfTest` and
	 * `FPM.Wrist.SelfTest`: round-trip a KNOWN probe entry through the REAL store instead of
	 * asserting it works.
	 *
	 * What would make this FAIL, concretely: `Report()` silently drops a row (count stays 0 after
	 * a call that should have added one), `ExtractModKey` misparses the probe's own synthetic path
	 * (proving the parser broken on a case its own author controls), or the aggregate total does
	 * not match the sum of what was reported (the sink lying about its own arithmetic). Every
	 * check is non-mutating of ANYTHING but the probe's own rows, which are removed afterward -
	 * a self-test that leaves fake data in the table a real report would then quote is worse than
	 * not testing.
	 *
	 * @return true if every check passed.
	 */
	static bool SelfTest();

	/** Every attribution reported this session, for a caller that wants the raw rows (the residue
	 *  sentinel or a future crash-corpus join, per Goal 5's evidence-base list). Read-only. */
	static const TArray<FAttribution>& GetAttributions();

private:
	static TArray<FAttribution>& MutableAttributions();
};
