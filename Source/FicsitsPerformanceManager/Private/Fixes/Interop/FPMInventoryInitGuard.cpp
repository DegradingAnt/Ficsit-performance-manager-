// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMInventoryInitGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "FGInventoryComponent.h"

#include <atomic>

namespace
{
	/*
	 * Counters, split so the log answers the question that matters: is the SOURCE fix doing the work, or
	 * is the backstop carrying it? If GLateRepaired ever climbs while GInitialised stays flat, hook 1 is
	 * being bypassed — almost certainly a subclass overriding BeginPlay without calling Super — and that
	 * is a finding, not noise.
	 */
	std::atomic<int32> GInitialised{0};       // hook 1: sized at BeginPlay, before anything could use it
	std::atomic<int32> GLateRepaired{0};      // hook 2: sized at AddStack, because hook 1 missed it
	std::atomic<int32> GRefused{0};           // off the game thread — left to vanilla. Expected: ZERO.

	/*
	 * A zero-slot inventory seen where we refuse to mutate. Both are "believed unreachable", and both
	 * are counted rather than assumed, because the whole premise of the authority gate is that the bug
	 * is a joining-client race. If GAuthorityObserved ever climbs, that premise is wrong and the
	 * SaveGame-persistence question has to be answered before anything is repaired there.
	 */
	std::atomic<int32> GAuthorityObserved{0}; // zero-slot ON THE AUTHORITY — observed, never repaired
	std::atomic<int32> GAuthoredZero{0};      // the asset itself says zero slots — not ours to overrule
}

FFPMInventoryInitGuard& FFPMInventoryInitGuard::Get()
{
	static FFPMInventoryInitGuard Instance;
	return Instance;
}

void FFPMInventoryInitGuard::Arm()
{
	UFGInventoryComponent* Sample = GetMutableDefault<UFGInventoryComponent>();

	/*
	 * HOOK 1 — THE SOURCE FIX. AFTER, not before: vanilla's own BeginPlay is the thing that would size
	 * the inventory, so we can only tell it failed once it has finished trying.
	 *
	 * ⚠ AN "AFTER" HANDLER TAKES NO SCOPE, unlike every other handler in this mod. SML's
	 * AddHandlerAfter wants `TFunction<void(C*)>` exactly (NativeHookManager.h:525) — and that is not an
	 * inconsistency to work around, it is the API being honest: vanilla has already run, so there is no
	 * call left to cancel or override. Writing `auto& Scope` here is a compile error, which is the right
	 * place to find out.
	 */
	auto OnBeginPlay = [](UFGInventoryComponent* Self)
	{
		if (!Self || Self->GetSizeLinear() > 0) { return; }   // sized correctly — nothing to do

		/*
		 * ⚠⚠ NEVER RESIZE ON THE AUTHORITY — Resize() WRITES SaveGame STATE, AND THAT IS RESIDUE IN ANT'S
		 * SAVE. This is a ZERO RESIDUE violation and it was in the shipped 0.2.0.
		 *
		 * Vanilla says so itself, FGInventoryComponent.h:624-626:
		 *     /-* When we resize the inventory we save how much bigger or smaller the inventory was made *-/
		 *     UPROPERTY( SaveGame )
		 *     int32 mAdjustedSizeDiff;
		 * and mInventoryStacks is UPROPERTY(SaveGame) too (:652). Resize's own body is a link stub
		 * (FGInventoryComponent.cpp:54 `void UFGInventoryComponent::Resize(int32 newSize){ }`), so HOW MUCH
		 * it persists is not readable here — which is exactly why it must not run where saves are written.
		 * The standing law is PREVENTION, not cleanup: a removed mod cannot undo a field it already wrote.
		 *
		 * ★ AND THE GATE COSTS NOTHING, BECAUSE THE BUG IS CLIENT-ONLY BY THIS FIX'S OWN EVIDENCE.
		 * SunFry's report is "crash on JOIN, singleplayer fine", and the header's own mechanism paragraph
		 * says why: a latent action resumes on a later tick, and on a JOINING CLIENT components replicate
		 * before their owners finish initialising, while on the host the owner initialised long ago.
		 *
		 * ⚠ SO Side::Any WAS AN OVER-CORRECTION, MADE EARLIER THE SAME DAY. It was applied on the general
		 * rule "all the fixes should run on the server too" — but that rule is for fixes whose EFFECT
		 * reaches the client through replicated state. This is a client-local race. Widening it bought no
		 * coverage for a bug that provably does not occur there, and paid for it in save residue.
		 *
		 * The hook still ARMS everywhere, deliberately: arming is free (once per component) and it keeps
		 * the server able to REPORT a zero-slot inventory if one ever appears. What is gated is the
		 * MUTATION, not the observation.
		 */
		if (Self->HasAuthority())
		{
			const int32 N = ++GAuthorityObserved;
			if ((N == 1 || (N % FPMLog::ThrottleNotable) == 0)
			&& FPMDiag::IsOn(FPMDiag::EChannel::InventoryInit))
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] inventory-init: a ZERO-SLOT inventory on %s exists ON THE AUTHORITY (#%d). "
					     "NOT repairing — Resize() writes SaveGame state (mAdjustedSizeDiff) and that would be "
					     "residue in the save. This was believed impossible: the crash is a joining-client "
					     "race. If this line appears, the premise is wrong and it needs solving with the "
					     "persistence question answered first."),
					*GetNameSafe(Self->GetOwner()), N);
			}
			return;
		}

		/*
		 * mDefaultInventorySize is the AUTHORED slot count — UPROPERTY(EditDefaultsOnly), so it is present
		 * on every instance from its archetype without needing to replicate. That is exactly the number
		 * vanilla would have used, which is why this is a repair and not an invention.
		 *
		 * A component authored at zero is left alone. "This inventory holds nothing" is a legitimate thing
		 * to author, and inventing a slot for it here would be us deciding something the asset already
		 * decided. Hook 2 handles the case where something nonetheless tries to put an item in one.
		 */
		const int32 Authored = Self->mDefaultInventorySize;
		if (Authored <= 0) { return; }

		// Non-authority only: this client never writes the save, so the resize is process-lifetime state
		// that the server's own replicated data overwrites when it arrives.
		Self->Resize(Authored);

		const int32 N = ++GInitialised;
		if ((N == 1 || (N % FPMLog::ThrottleRoutine) == 0)
			&& FPMDiag::IsOn(FPMDiag::EChannel::InventoryInit))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] inventory-init: sized an uninitialised inventory on %s to its authored %d slot(s) "
				     "at BeginPlay (#%d) - it can no longer reach the 'Inventory need to be initialized' assert"),
				*GetNameSafe(Self->GetOwner()), Authored, N);
		}
	};

	FPM_SUBSCRIBE_VIRTUAL_AFTER("inventory-init", UFGInventoryComponent::BeginPlay, Sample, OnBeginPlay);

	/*
	 * HOOK 2 — THE BACKSTOP. Ant: "we still need a guard IF the source fix ever fails".
	 */
	auto OnAddStack = [](auto& Scope, UFGInventoryComponent* Self, const FInventoryStack& Stack, const bool bAllowPartialAdd)
	{
		/*
		 * IsValidIndex(0) IS THE ASSERT'S OWN CONDITION (FGInventoryComponent.h:229-232, `idx < Num()`),
		 * public and FORCEINLINE, so the common case costs one integer compare and no call.
		 */
		if (!Self || Self->IsValidIndex(0)) { return; }   // a real inventory — vanilla behaviour untouched

		/*
		 * ★ THE GOVERNING RULE FOR EVERY BRANCH BELOW: NEVER MAKE IT WORSE THAN VANILLA. Repair only where
		 * the repair is free of cost; everywhere else, log and get out of the way so the outcome is
		 * exactly what an unmodded game would have done.
		 *
		 * ⚠ THERE IS NO `Scope.Override(0)` IN THIS FUNCTION ANY MORE, AND THAT IS THE POINT.
		 * 0.2.0 had two of them. Returning "I accepted 0 items" reads as safe and is not: the ordinary
		 * caller shape is REMOVE FROM SOURCE, THEN ADD TO DESTINATION, so a 0 after the removal means the
		 * items exist nowhere. Ant: "we cant delete items. illegal." Falling through instead hands the
		 * decision back to vanilla — which for a zero-slot inventory means its assert. That is loud,
		 * attributable, recoverable, and it destroys nothing.
		 */

		/*
		 * AUTHORITY: OBSERVE, NEVER MUTATE. Resize() writes SaveGame state (mAdjustedSizeDiff,
		 * FGInventoryComponent.h:624-626) and its body is a link stub, so the persistence cost is not
		 * readable. ZERO RESIDUE is prevention, not cleanup — a removed mod cannot unwrite a saved field.
		 */
		if (Self->HasAuthority())
		{
			const int32 N = ++GAuthorityObserved;
			if (!FPMDiag::IsOn(FPMDiag::EChannel::InventoryInit)) { return; }
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] inventory-init: AddStack hit a ZERO-SLOT inventory on %s ON THE AUTHORITY (#%d). "
				     "Falling through to vanilla unchanged - repairing here would write SaveGame state, and "
				     "refusing would destroy the caller's items. Believed unreachable (the crash is a "
				     "joining-client race); if this appears, it needs solving with evidence, not a guard."),
				*GetNameSafe(Self->GetOwner()), N);
			return;
		}

		/*
		 * OFF THE GAME THREAD: cannot Resize (it reallocates and broadcasts ResizeInventoryDelegate).
		 * Fall through rather than refuse — same reasoning as above, items outrank a crash.
		 */
		if (!IsInGameThread())
		{
			const int32 N = ++GRefused;
			if (!FPMDiag::IsOn(FPMDiag::EChannel::InventoryInit)) { return; }
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] inventory-init: AddStack reached a ZERO-SLOT inventory on %s from OFF the game "
				     "thread (#%d). Cannot Resize safely, so vanilla runs unchanged. This branch was believed "
				     "unreachable; it needs a game-thread hop, not a wider guard."),
				*GetNameSafe(Self->GetOwner()), N);
			return;
		}

		/*
		 * AUTHORED AT ZERO: leave it alone, exactly as hook 1 does.
		 *
		 * 0.2.0 gave it one slot here, on the argument that something was actively trying to store an item.
		 * That contradicted hook 1's own stated principle four lines away — "inventing a slot for it here
		 * would be us deciding something the asset already decided" — and the review was right to call it
		 * unasked-for widening. An asset authored to hold nothing is not a bug for us to overrule.
		 */
		const int32 Authored = Self->mDefaultInventorySize;
		if (Authored <= 0)
		{
			const int32 N = ++GAuthoredZero;
			if ((N == 1 || (N % FPMLog::ThrottleNotable) == 0)
			&& FPMDiag::IsOn(FPMDiag::EChannel::InventoryInit))
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] inventory-init: something is adding to an inventory on %s that its own asset "
					     "authored with ZERO slots (#%d). Left alone - vanilla decides. The caller is the bug."),
					*GetNameSafe(Self->GetOwner()), N);
			}
			return;
		}

		// Non-authority, game thread, real authored size: repair and fall through. This client never
		// writes the save, so the resize is process-lifetime state the server's data overwrites on arrival.
		Self->Resize(Authored);

		/*
		 * No reentrancy guard is needed even though Resize broadcasts: a re-entered call finds
		 * IsValidIndex(0) true at the top and returns immediately, so the recursion cannot continue.
		 *
		 * ⚠ THE LOG DOES NOT CLAIM AN OUTCOME VANILLA HAS NOT PRODUCED YET. 0.2.0 said "no items lost"
		 * from here — but this runs BEFORE vanilla's AddStack, so at this point nothing has been stored
		 * and the claim was unknowable. It reports what it actually did: gave the inventory its slots.
		 */
		const int32 N = ++GLateRepaired;
		if (!FPMDiag::IsOn(FPMDiag::EChannel::InventoryInit)) { return; }
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] inventory-init: AddStack hit an UNINITIALISED inventory on %s that BeginPlay did not "
			     "size (#%d). Gave it its authored %d slot(s); vanilla's AddStack now runs against a real "
			     "array. A subclass overriding BeginPlay without calling Super produces exactly this."),
			*GetNameSafe(Self->GetOwner()), N, Authored);
		FPMOverlay::Post(TEXT("inventory-init"),
			FString::Printf(TEXT("sized-at-BeginPlay %d · late-repaired %d · authored-zero %d · authority-seen %d"),
				GInitialised.load(), N, GAuthoredZero.load(), GAuthorityObserved.load()));
	};

	FPM_SUBSCRIBE_VIRTUAL("inventory-init", UFGInventoryComponent::AddStack, Sample, OnAddStack);
}
