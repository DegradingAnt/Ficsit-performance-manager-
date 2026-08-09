// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * INVENTORY INIT REPAIR — an inventory that arrives with no slots gets its slots, instead of eating
 * what someone tried to put in it.
 *
 * THE CRASH (SunFry, message(2).txt 2026-08-03 — crash on JOIN, singleplayer fine):
 *   Assertion failed: mInventoryStacks.Num() > 0   FGInventoryComponent.cpp:591
 *   "Inventory need to be initialized before use 0"
 *     UFGInventoryComponent::AddStack()
 *     UFGInventoryComponent::execAddStack()             <- the BLUEPRINT VM, not C++
 *     FLatentActionManager::TickLatentActionForObject() <- a latent node (Delay/Timeline/Retriggerable)
 *
 * A latent action resumes on a LATER tick, so it fires against whatever state exists then. On a joining
 * client that window is wide open: components replicate before their owners finish initialising. On the
 * host the owner initialised long ago, which is why Ant's client is fine on the same build and save.
 *
 * ⚠ AND IT IS NOT ONLY A CRASH. A crash DURING join rebinds a player to a fresh character and orphans
 * the one holding their inventory — the same second-order damage as the hologram guard, and it has been
 * paid for eight times.
 *
 * ★ WHY THIS IS A REPAIR AND NOT A REFUSAL. THE OLD FIX COULD DESTROY ITEMS.
 *
 * The old mod answered `Scope.Override(0)` — "I accepted none of them". That reads as safe and is not,
 * because the usual caller shape is *remove from source, then add to destination*: if the removal
 * already happened, returning 0 means the items exist nowhere. On a pure client that is cosmetic, since
 * the server corrects it. ON A LISTEN HOST THE CLIENT IS THE AUTHORITY, so the loss is real and
 * permanent — and the old guard was gated on process type, not authority, so it took that branch on
 * exactly the machine that owns the save.
 *
 * Ant, 2026-08-08: *"we cant delete items. illegal. we need to not make zero inventory crates at all.
 * it needs to be fixed at the source, not a bandaid solution. we still need a guard IF the source fix
 * ever fails, but still"*
 *
 * ★ SO THERE ARE TWO HOOKS, AND THE FIRST ONE IS THE ACTUAL FIX.
 *
 * 1. SOURCE — `UFGInventoryComponent::BeginPlay` (AFTER). If the component finishes BeginPlay with zero
 *    slots, give it the size it was authored with (`mDefaultInventorySize`, FGInventoryComponent.h:622)
 *    via the public `Resize()` (:203).
 *
 * 2. BACKSTOP — `UFGInventoryComponent::AddStack`. The guard Ant asked to keep in case the source fix
 *    misses one.
 *
 * ⚠ WHAT HOOK 1 ACTUALLY GUARANTEES, stated precisely because 0.2.0 overstated it. It said "after this
 * an inventory is never zero-slot in the first place, so AddStack cannot reach the assert" — and then
 * described, four lines later, two ways it still can. The true statement is narrower: **an inventory
 * that was AUTHORED with slots, on a non-authority client, is not zero-slot when AddStack runs.** THREE
 * populations remain by design, which is exactly why hook 2 exists:
 *   - inventories authored at zero — left alone deliberately; the asset decided that, not us,
 *   - subclasses overriding BeginPlay without calling Super — `SUBSCRIBE_METHOD_VIRTUAL` patches only
 *     the class it is given, and that is not checkable from the SML stub tree,
 *   - anything on the AUTHORITY — see the side note below.
 * This is a repair at the earliest reachable choke point, NOT a fix at the origin. Whether that
 * satisfies "fixed at the source" is Ant's call, and it is stated plainly here so she can make it.
 *
 * ★ NOTHING IS EVER DESTROYED. There is no `Scope.Override(0)` anywhere in this fix. 0.2.0 had two, and
 * "I accepted 0 items" destroys the stack whenever the caller already removed it from the source. Every
 * path this fix declines to repair now FALLS THROUGH to vanilla unchanged — the outcome an unmodded
 * game would produce, which is loud and recoverable rather than silent and permanent.
 *
 * ⚠⚠ SIDE IS `Any` SO IT ARMS AND OBSERVES EVERYWHERE, BUT IT NEVER MUTATES ON THE AUTHORITY.
 * `Resize()` writes SaveGame state — vanilla's own header, FGInventoryComponent.h:624-626: "When we
 * resize the inventory we save how much bigger or smaller the inventory was made", `UPROPERTY(SaveGame)
 * int32 mAdjustedSizeDiff`. Its body is a link stub, so how much it persists is not readable here.
 * Repairing on the authority would therefore write residue into Ant's save, and ZERO RESIDUE is
 * prevention, not cleanup — an uninstalled mod cannot unwrite a saved field.
 *
 * That costs no coverage, because THE BUG IS CLIENT-ONLY BY THIS FIX'S OWN EVIDENCE: "crash on JOIN,
 * singleplayer fine", and the mechanism above says why — on a joining client components replicate
 * before their owners finish initialising, while on the host the owner initialised long ago. An earlier
 * pass widened this to the authority on the general rule "all the fixes should run on the server too";
 * that rule is for fixes whose EFFECT reaches clients through replicated state, and this is a local
 * race. The widening bought nothing and paid in save residue.
 */
class FFPMInventoryInitGuard final : public IFPMFix
{
public:
	static FFPMInventoryInitGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("inventory-init"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** ChokePointRepair: the CREATOR of zero-slot inventories is still unnamed. Ant's ruling attaches the origin-naming
	 * diagnostic to this fix: log the owner class of every inventory it resizes. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::ChokePointRepair; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::InventoryInit; }
	virtual void Arm() override;
};
