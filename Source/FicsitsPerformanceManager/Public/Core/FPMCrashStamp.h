// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * CRASH STAMP — make a dump say what FPM was, BEFORE anything crashes.
 *
 * ★ WHY THIS IS REGISTERED AT ARM TIME AND NOT WRITTEN AT CRASH TIME. That distinction is the whole
 * design, and it was bought with a failure: the schematic breadcrumb wrote its state when the crash
 * happened, via `OnHandleSystemError`, and went **0-for-3 on access violations in Shipping** — that
 * handler simply never fires for an AV. Dump `A981D1D4` is the receipt: the same crash, with no mod
 * information in it at all, because the code that would have written it never ran.
 *
 * `FGenericCrashContext::SetGameData` is the other shape. It stores the key/value NOW, in the crash
 * context object the reporter already carries, so the value is present in the dump no matter how
 * violently the process dies. Nothing of ours has to execute at crash time.
 *
 * ★ THE API IS SHIPPING-SAFE, AND THAT WAS CHECKED RATHER THAN HOPED. `SetGameData` is declared
 * `CORE_API static void SetGameData(const FString&, const FString&)`
 * (`GenericPlatformCrashContext.h:627`) with no `#if` guard around it. And the design's own survey of
 * the crash corpus found a populated `<GameData>` block in 30 of 31 dumps, including all four
 * current-build GPU-crash dumps — so this mechanism is known to work on THIS game, from real dumps,
 * not from the header alone.
 *
 * ⚠ WHY THAT CAVEAT IS SPELLED OUT: on 2026-08-10 a cvar declared unguarded in this same engine tree
 * came back "Command not recognized" from the shipped game. The engine source describes what the
 * ENGINE can do, not what this BINARY has. `SetGameData` gets past that objection only because the
 * dumps themselves are the evidence.
 *
 * WHAT IT STAMPS: the plugin version, the armed-fix roster with each fix's origin status and side,
 * the installed-hook count, and which side this process is. That is exactly the set a reader of a
 * future dump needs in order to answer "was FPM in this, and doing what" without having to find the
 * matching FactoryGame.log — which for a server crash usually no longer exists by the time anyone
 * looks.
 *
 * VIEWER ONLY. It reads its own registries and writes strings into a diagnostic sink. It changes no
 * cvar, hooks nothing, touches no vanilla state, and does no network I/O.
 */
class FICSITSPERFORMANCEMANAGER_API FPMCrashStamp
{
public:
	/**
	 * Register the whole stamp. Call ONCE, from StartupModule, AFTER every fix has armed — the roster
	 * is the point, and stamping before arming would record an empty one.
	 */
	static void Register(const FString& VersionName);

	/** `FPM.CrashStamp` — print what was stamped, so the stamp itself can be verified without a crash. */
	static void LogStamped();
};
