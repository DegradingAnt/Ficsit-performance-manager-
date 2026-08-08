// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Patching/NativeHookManager.h"

/**
 * ONE INSTALLED HOOK, RECORDED AT INSTALL TIME.
 *
 * Owner and Target are string LITERALS, never owned strings — they come from the macro below and live
 * for the life of the process, so the ledger stores pointers and copies nothing.
 */
struct FPMHookRecord
{
	const TCHAR* Owner = nullptr;
	const TCHAR* Target = nullptr;
	int32 Order = INDEX_NONE;
	bool bInstalled = false;
};

/**
 * THE HOOK LEDGER — every native hook FPM installs goes through it, and it exists because the old mod
 * could not answer a question it should have owned.
 *
 * Roughly 168-180 native hooks live in Ant's stack. When something broke, FPM could not say which of
 * them were its own, in what order they installed, or which target each one sat on. SML installs hooks
 * in registration order (hooking.adoc:43), so install order is not a curiosity — it decides who can
 * cancel whom. An inventory that is derived from the install itself cannot drift from reality; a
 * hand-maintained list would.
 *
 * THIS IS A LEDGER, NOT A FRAMEWORK, AND IT MUST STAY THAT WAY. It records and it gates the editor. It
 * does not own lifetimes, deduplicate targets, or arbitrate collisions — no hook collision has ever
 * been observed in this project, and building for one would be building for an imagined problem.
 *
 * THREADING: install happens from StartupModule on the game thread, before any world exists. The array
 * is not guarded because nothing else can reach it at that point. If a hook is ever installed off the
 * game thread, that assumption breaks and this needs a lock — do not quietly install one instead.
 */
class FICSITSPERFORMANCEMANAGER_API FPMHookLedger
{
public:
	/**
	 * Runs the installer, records the result, and returns whatever the SML macro returned.
	 *
	 * Refuses to install in the editor and says so. hooking.adoc:109-125: hook behaviour at editor time
	 * is unpredictable and can leave you unable to open the editor until you edit the code externally.
	 * The gate is a runtime `if constexpr` rather than a `#if` on purpose — the same page warns that
	 * `#if` hides errors until a shipping build and confuses IDEs, so the handler still COMPILES here,
	 * it simply never arms.
	 */
	static FDelegateHandle Install(const TCHAR* Owner, const TCHAR* Target, TFunctionRef<FDelegateHandle()> Installer);

	/** Everything installed so far, in install order. */
	static const TArray<FPMHookRecord>& Records();

	/** Prints the whole inventory. Called once, after arming, so a user's log always carries it. */
	static void LogInventory();
};

/*
 * THE WRAPPER MACROS.
 *
 * The target string is stringified from the SAME token that is passed to SML, so the ledger's Target
 * column cannot drift away from the function actually hooked. A separate string literal could, and
 * would, silently.
 *
 * WHY THERE IS NO GENERIC "PASS ME ANY SUBSCRIBE EXPRESSION" FORM: SML's macros differ in arity, and
 * one wrapper per variant is what keeps the stringify honest. Add a variant when a fix needs one.
 * Do NOT reach past these for a raw SUBSCRIBE_ — a hook that skips the ledger is a hook the inventory
 * lies about, which is worse than no inventory.
 *
 * !!! THE HANDLER MUST NOT CONTAIN A TOP-LEVEL COMMA. !!!
 * SML's SUBSCRIBE_ macros are function-like macros, so the preprocessor splits the handler on commas
 * and does not treat angle brackets as grouping. Both of these are hard, non-obvious errors:
 *     int32 A = 0, B = 0;                             -> "too many arguments to macro"
 *     for (const TPair<EOnlineServices, FAccountId>&  -> same, on the template's comma
 * `__VA_ARGS__` here cannot save you: it re-expands into the inner macro as separate arguments.
 *
 * THE FIX IS TO NAME THE LAMBDA FIRST and pass the name, which is what FPM does:
 *     auto OnJoin = [](auto& Scope, AFGGameMode* Self, APlayerController* PC) { ...commas fine... };
 *     FPM_SUBSCRIBE_VIRTUAL("my-fix", AFGGameMode::FindInactivePlayer, Sample, OnJoin);
 * The lambda body is then never inside a macro at all and the trap disappears rather than being
 * worked around every time.
 */
#define FPM_SUBSCRIBE(Owner, MethodReference, ...) \
	FPMHookLedger::Install(TEXT(Owner), TEXT(#MethodReference), \
		[&]() -> FDelegateHandle { return SUBSCRIBE_METHOD(MethodReference, __VA_ARGS__); })

#define FPM_SUBSCRIBE_VIRTUAL(Owner, MethodReference, SampleObject, ...) \
	FPMHookLedger::Install(TEXT(Owner), TEXT(#MethodReference), \
		[&]() -> FDelegateHandle { return SUBSCRIBE_METHOD_VIRTUAL(MethodReference, SampleObject, __VA_ARGS__); })

#define FPM_SUBSCRIBE_VIRTUAL_AFTER(Owner, MethodReference, SampleObject, ...) \
	FPMHookLedger::Install(TEXT(Owner), TEXT(#MethodReference), \
		[&]() -> FDelegateHandle { return SUBSCRIBE_METHOD_VIRTUAL_AFTER(MethodReference, SampleObject, __VA_ARGS__); })
