// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ MAKE A REPORT COMMAND ANSWER WHERE THE OPERATOR IS LOOKING.
 *
 * Ant, 2026-08-10, having typed three of FPM's own report commands into the console:
 * *"i did them and they did nothing visibly"*. All three had run. All three had written full answers.
 * Into `FactoryGame.log`, which she was not reading, while the console she typed into stayed blank.
 *
 * ══ THE MECHANISM, BECAUSE IT IS NOT OBVIOUS AND IT COST A BOOT ══
 *
 * `UE_LOG(..., Display, ...)` does NOT echo to the in-game console. A console command registered as
 * `FAutoConsoleCommand` gets no output device at all, so a command whose body is entirely `UE_LOG` is
 * invisible from the console by construction — it looks broken and is working perfectly.
 * `sf-boottest` names this exact trap, and three commands shipped with it anyway.
 *
 * ══ WHY THIS AND NOT AN `FOutputDevice*` PARAMETER ══
 *
 * The parameter is what `FPMCVarWriter::LogLedger` and `FPMUserSettingMap::LogState` already do, and it
 * is correct for them — they emit a handful of lines. The three reports that were silent emit dozens
 * between them, across branches, and threading a device through every one is a large diff whose only
 * risk is missing a line and leaving the same bug half-fixed.
 *
 * This attaches the console's device to `GLog` for the duration of the call instead, so EVERY line the
 * report writes reaches the console, including lines added later by someone who never heard of this
 * class. It fixes the category rather than the three instances.
 *
 * ⚠ IT OVER-CAPTURES, DELIBERATELY, AND HERE IS THE BOUND. While the scope is alive, anything else
 * logging on any thread also lands in the console. A report takes microseconds, so the realistic
 * over-capture is a stray line or two — and a stray line in a diagnostic dump is a far cheaper failure
 * than the one this replaces, where the answer did not appear at all.
 *
 * ⚠ NULL IS A NO-OP, not an error. Internal callers pass nullptr and keep log-only behaviour, which is
 * what the shutdown path and the automatic summaries want.
 */
struct FICSITSPERFORMANCEMANAGER_API FPMScopedConsoleEcho
{
	explicit FPMScopedConsoleEcho(class FOutputDevice* InAr);
	~FPMScopedConsoleEcho();

	/** Not copyable or movable — it owns a registration on a global for a scope. */
	FPMScopedConsoleEcho(const FPMScopedConsoleEcho&) = delete;
	FPMScopedConsoleEcho& operator=(const FPMScopedConsoleEcho&) = delete;

private:
	class FOutputDevice* Attached = nullptr;
};
