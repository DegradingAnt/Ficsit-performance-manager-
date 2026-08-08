// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMInventoryInitGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMHookLedger.h"
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
	std::atomic<int32> GInitialised{0};   // hook 1: sized at BeginPlay, before anything could use it
	std::atomic<int32> GLateRepaired{0};  // hook 2: sized at AddStack, because hook 1 missed it
	std::atomic<int32> GRefused{0};       // hook 2: could not repair. Expected: ZERO, forever.
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

		Self->Resize(Authored);

		const int32 N = ++GInitialised;
		if (N == 1 || (N % 200) == 0)
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
		 * ⚠ THE ONE PATH THAT CANNOT REPAIR, AND IT IS A THREADING PATH.
		 *
		 * Resize() reallocates the stack array and broadcasts ResizeInventoryDelegate, so it is game-thread
		 * work. AddStack is game-thread by construction (the reported crash came through
		 * FLatentActionManager, which is), and this branch has never been observed — but "never observed"
		 * is not "cannot happen", and this project has already been bitten by state reachable from Factory
		 * Tick workers.
		 *
		 * So it logs at Error and says out loud that this is the only branch where items can be refused,
		 * rather than returning silently and leaving a reader to discover it. If this line ever appears,
		 * the fix is wrong and needs a game-thread hop, not a wider guard.
		 */
		if (!IsInGameThread())
		{
			Scope.Override(0);
			const int32 N = ++GRefused;
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] inventory-init: AddStack reached a ZERO-SLOT inventory on %s from OFF the game "
				     "thread (#%d). Cannot Resize safely here, so the add was refused - THE CALLER MAY HAVE "
				     "LOST THE ITEMS. This branch was believed unreachable; it needs a game-thread hop."),
				*GetNameSafe(Self->GetOwner()), N);
			return;
		}

		/*
		 * REPAIR, DO NOT REFUSE. Take the authored size if there is one, otherwise one slot — enough for
		 * the stack that is arriving. Either way the items have somewhere to go, which is the whole point:
		 * "we cant delete items. illegal."
		 *
		 * Giving one slot to an inventory authored at zero is a deliberate asymmetry with hook 1. There we
		 * are asked nothing and change nothing; here something is actively trying to store an item, and
		 * between crashing, eating it, and widening a container by one slot, the last is the only option
		 * that loses neither the session nor the item.
		 */
		const int32 Want = FMath::Max(Self->mDefaultInventorySize, 1);
		Self->Resize(Want);

		if (Self->IsValidIndex(0))
		{
			/*
			 * Fall through — vanilla's AddStack runs against a real array and stores the items itself. No
			 * Override, so we never have to reimplement its partial-add / stacking bookkeeping, and its
			 * return value stays honest.
			 *
			 * No reentrancy guard is needed even though Resize broadcasts: a re-entered call finds
			 * IsValidIndex(0) true at the top and returns immediately, so the recursion cannot continue.
			 */
			const int32 N = ++GLateRepaired;
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] inventory-init: AddStack hit an UNINITIALISED inventory on %s that BeginPlay did "
				     "not size (#%d). Gave it %d slot(s) and let the add proceed - no items lost, no assert. "
				     "A subclass overriding BeginPlay without calling Super would produce exactly this."),
				*GetNameSafe(Self->GetOwner()), N, Want);
			FPMOverlay::Post(TEXT("inventory-init"),
				FString::Printf(TEXT("%d sized at BeginPlay, %d late-repaired at AddStack"),
					GInitialised.load(), N));
			return;
		}

		/*
		 * Unreachable by construction — Resize(>=1) cannot leave zero slots. Kept because the alternative
		 * to a guard here is falling through into the assert, and a comment claiming impossibility is not
		 * a reason to hand a crash to a joining player.
		 */
		Scope.Override(0);
		const int32 N = ++GRefused;
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] inventory-init: Resize(%d) left %s with zero slots (#%d) - refused the add. This is "
			     "supposed to be impossible; the items may be lost and the cause is upstream of this fix."),
			Want, *GetNameSafe(Self->GetOwner()), N);
	};

	FPM_SUBSCRIBE_VIRTUAL("inventory-init", UFGInventoryComponent::AddStack, Sample, OnAddStack);
}
