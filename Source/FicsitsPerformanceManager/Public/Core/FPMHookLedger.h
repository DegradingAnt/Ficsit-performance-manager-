// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformAtomics.h"
#include "Templates/Function.h"
#include "Patching/NativeHookManager.h"

/**
 * ONE CALL SITE'S REACHED COUNTER.
 *
 * THE PROBLEM THIS SOLVES. The ledger could say that an install returned a handle. It could not say
 * that the handler ever ran. Those are different claims, and only the second one answers "is this fix
 * doing anything". A grep for REACHED across the whole Source tree returned zero files before this
 * struct existed, so every armed line in the log was an install receipt and nothing more.
 *
 * ONE OF THESE EXISTS PER CALL SITE, allocated by FPMHookLedger::Install and owned by the ledger for
 * the life of the process. The address is stable, because the counting wrapper captures the pointer.
 *
 * THREADING. Some hooked functions run off the game thread. UNetDriver::ProcessRemoteFunction and the
 * Factory Tick workers are both real cases in this tree. Every write goes through FPlatformAtomics.
 *
 * Reached is int64 because a hot hook can pass 2^31 in a long session. ProcessRemoteFunction sees
 * every RPC in the game.
 */
struct FPMHookCounter
{
	/** How many times the handler at this call site has run. */
	volatile int64 Reached = 0;

	/**
	 * Set to 1 by FPMHookCount::Wrap at install time.
	 *
	 * ZERO MEANS THE CALL SITE NEVER GOT A COUNTING WRAPPER, so its Reached value can only ever be 0
	 * and a reader must not read that 0 as "the handler never ran". This is the flag that keeps a
	 * dead instrument from looking like a clean bill of health.
	 */
	volatile int32 bWrapped = 0;
};

/**
 * THE COUNTING WRAPPER.
 *
 * SML's Handler type is TFunction<void(ScopeType&, ArgumentTypes...)> (NativeHookManager.h:254) and
 * AddHandlerBefore takes Handler&& (lines 339 and 515). A generic variadic lambda converts to that
 * TFunction, so one wrapper covers every handler signature SML generates: the BEFORE shape
 * (ScopeType&, Args...), the AFTER shape for a void return (Args...), and the AFTER shape for a
 * non-void return (const Ret&, Args...) from HandlerAfterFunc at line 233.
 *
 * ⚠ THE HANDLER IS CAPTURED BY VALUE, AND THAT IS NOT A STYLE CHOICE. SML stores the TFunction in a
 * TSharedPtr that outlives the installer lambda. A by-reference capture would dangle, and it would
 * dangle silently.
 */
namespace FPMHookCount
{
	/**
	 * Returns a callable that increments Slot and then calls Handler with every argument unchanged.
	 *
	 * ⚠ INSTALLING MUST NOT MOVE THE COUNT. Wrap marks the slot as wrapped and nothing else. An
	 * instrument that counts its own installation reports 1 on a hook that never fired.
	 *
	 * ⚠ THE COST, STATED RATHER THAN IMPLIED: one locked increment per hooked call, on a cache line
	 * shared by every thread that reaches that call site. For AFGBuildable::BeginPlay that is a few
	 * thousand increments at world load and nothing after. The one call site where this could matter
	 * is UNetDriver::ProcessRemoteFunction, which carries every RPC in the game. That fix
	 * (no-owner-rpc-gate) returns false from DefaultArmed and installs nothing unless a player turns
	 * it on. This cost is NOT measured. It is an argument from the instruction count, and the way to
	 * settle it is a frame-time comparison with the gate on, not another argument.
	 */
	template <typename HandlerType>
	auto Wrap(FPMHookCounter* Slot, HandlerType Handler)
	{
		if (Slot)
		{
			FPlatformAtomics::InterlockedExchange(&Slot->bWrapped, 1);
		}

		return [Slot, Handler](auto&&... Args)
		{
			if (Slot)
			{
				FPlatformAtomics::InterlockedIncrement(&Slot->Reached);
			}
			Handler(Forward<decltype(Args)>(Args)...);
		};
	}
}

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

	/** This call site's REACHED slot. Owned by the ledger. Never null after Install returns. */
	FPMHookCounter* Counter = nullptr;
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
 * The REACHED counters are a different case and they ARE guarded, because handlers run wherever the
 * hooked function runs. See FPMHookCounter.
 */
class FICSITSPERFORMANCEMANAGER_API FPMHookLedger
{
public:
	/**
	 * Allocates this call site's REACHED slot, runs the installer, records the result, and returns
	 * whatever the SML macro returned.
	 *
	 * The installer receives the slot. The macros below hand it to FPMHookCount::Wrap, which is how a
	 * handler gets counted. Nothing else may call this function: tools/check_structure.py fails the
	 * build if a .cpp calls FPMHookLedger::Install directly, because a hand-written call can skip the
	 * wrapper and the ledger would then report a permanent 0 that reads like a silent hook.
	 *
	 * Refuses to install in the editor and says so. hooking.adoc:109-125: hook behaviour at editor time
	 * is unpredictable and can leave you unable to open the editor until you edit the code externally.
	 * The gate is a runtime `if constexpr` rather than a `#if` on purpose — the same page warns that
	 * `#if` hides errors until a shipping build and confuses IDEs, so the handler still COMPILES here,
	 * it simply never arms.
	 */
	static FDelegateHandle Install(const TCHAR* Owner, const TCHAR* Target,
		TFunctionRef<FDelegateHandle(FPMHookCounter*)> Installer);

	/** Everything installed so far, in install order. */
	static const TArray<FPMHookRecord>& Records();

	/**
	 * Prints the whole inventory, the REACHED count of every hook, and the coverage line.
	 *
	 * Called once after arming, so a user's log always carries it, and again on demand through
	 * `FPM.Hooks.Report`. THE ON-DEMAND ROUTE IS THE ONE THAT ANSWERS ANYTHING: at boot every REACHED
	 * count is necessarily 0, because no hooked function has run yet.
	 */
	static void LogInventory();

	/**
	 * True when this slot carries a counting wrapper.
	 *
	 * The one predicate AuditCountingWrappers is built from. It is exposed so the boot self-test can
	 * prove it against a known-positive and a known-negative instead of asserting that it works.
	 */
	static bool IsCounted(const FPMHookCounter& Counter);

	/**
	 * Proves that every INSTALLED hook got a counting wrapper. Logs an Error naming each offender.
	 *
	 * ⚠ WHAT MAKES THIS A REAL CHECK AND NOT A TAUTOLOGY: a new FPM_SUBSCRIBE variant that forgets
	 * FPMHookCount::Wrap compiles clean and installs a working hook. Its REACHED count then stays 0
	 * forever, which is exactly the shape of "this handler never runs". This is the check that tells
	 * the two apart.
	 *
	 * @return true if every installed hook is counted.
	 */
	static bool AuditCountingWrappers();
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
 * ⚠ A NEW VARIANT MUST PASS ITS HANDLER THROUGH FPMHookCount::Wrap. tools/check_structure.py reads
 * this header and fails the build on a FPM_SUBSCRIBE macro whose body calls a SUBSCRIBE_ macro
 * without Wrap. A variant that forgets it installs a real hook whose REACHED count can never move.
 *
 * !!! THE HANDLER MUST NOT CONTAIN A TOP-LEVEL COMMA. !!!
 * SML's SUBSCRIBE_ macros are function-like macros, so the preprocessor splits the handler on commas
 * and does not treat angle brackets as grouping. Both of these are hard, non-obvious errors:
 *     int32 A = 0, B = 0;                             -> "too many arguments to macro"
 *     for (const TPair<EOnlineServices, FAccountId>&  -> same, on the template's comma
 *
 * THE FIX IS TO NAME THE LAMBDA FIRST and pass the name:
 *     auto OnJoin = [](auto& Scope, AFGGameMode* Self, APlayerController* PC) { ...commas fine... };
 *     FPM_SUBSCRIBE_VIRTUAL("my-fix", AFGGameMode::FindInactivePlayer, Sample, OnJoin);
 * The lambda body is then never inside a macro at all and the trap disappears rather than being
 * worked around every time.
 *
 * ★ THE HANDLER PARAMETER IS NAMED, NOT `__VA_ARGS__`, AND THAT CHANGE IS LOAD-BEARING. The counting
 * wrapper needs ONE handler expression to wrap. `__VA_ARGS__` re-expands as separate arguments, so
 * there is no single token to hand to FPMHookCount::Wrap. It also hid the comma trap one level
 * deeper: a split handler used to fail inside SML's macro instead of here. Every call site in this
 * mod now passes a named lambda. FPMWwiseServerGate.cpp and FPMWireNullGuard.cpp were the last two
 * inline ones and both were converted with this change.
 *
 * ⚠ THE WRAP CALL IS PARENTHESISED. `FPMHookCount::Wrap(Slot, Handler)` contains a comma. The
 * enclosing parentheses are what stop the preprocessor from splitting SML's macro on it.
 */
#define FPM_SUBSCRIBE(Owner, MethodReference, Handler) \
	FPMHookLedger::Install(TEXT(Owner), TEXT(#MethodReference), \
		[&](FPMHookCounter* FPMHookSlot) -> FDelegateHandle \
		{ \
			return SUBSCRIBE_METHOD(MethodReference, (FPMHookCount::Wrap(FPMHookSlot, Handler))); \
		})

/**
 * AFTER variant of FPM_SUBSCRIBE, for a non-virtual (free or static member) function. Added for
 * join-version-echo, which has to read a function's OUTPUT (an out-param already filled by the real
 * call) rather than intercept its input — none of the three wrappers above cover that, and this is the
 * "add a variant when a fix needs one" case the note above already invites.
 */
#define FPM_SUBSCRIBE_AFTER(Owner, MethodReference, Handler) \
	FPMHookLedger::Install(TEXT(Owner), TEXT(#MethodReference), \
		[&](FPMHookCounter* FPMHookSlot) -> FDelegateHandle \
		{ \
			return SUBSCRIBE_METHOD_AFTER(MethodReference, (FPMHookCount::Wrap(FPMHookSlot, Handler))); \
		})

#define FPM_SUBSCRIBE_VIRTUAL(Owner, MethodReference, SampleObject, Handler) \
	FPMHookLedger::Install(TEXT(Owner), TEXT(#MethodReference), \
		[&](FPMHookCounter* FPMHookSlot) -> FDelegateHandle \
		{ \
			return SUBSCRIBE_METHOD_VIRTUAL(MethodReference, SampleObject, \
				(FPMHookCount::Wrap(FPMHookSlot, Handler))); \
		})

#define FPM_SUBSCRIBE_VIRTUAL_AFTER(Owner, MethodReference, SampleObject, Handler) \
	FPMHookLedger::Install(TEXT(Owner), TEXT(#MethodReference), \
		[&](FPMHookCounter* FPMHookSlot) -> FDelegateHandle \
		{ \
			return SUBSCRIBE_METHOD_VIRTUAL_AFTER(MethodReference, SampleObject, \
				(FPMHookCount::Wrap(FPMHookSlot, Handler))); \
		})
