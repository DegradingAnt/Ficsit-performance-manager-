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
 *    via the public `Resize()` (:203). After this an inventory is never zero-slot in the first place, so
 *    AddStack cannot reach the assert. That is "not making zero inventory crates at all".
 *
 * 2. BACKSTOP — `UFGInventoryComponent::AddStack`. The guard Ant asked to keep in case the source fix
 *    misses one. It no longer refuses: it repairs in place and lets vanilla's AddStack run, so the items
 *    land in real slots. NOTHING IS DESTROYED on any path that can be reached in practice.
 *
 * WHY THE BACKSTOP CANNOT SIMPLY BE DELETED: `SUBSCRIBE_METHOD_VIRTUAL` patches only the class it is
 * given. Any UFGInventoryComponent subclass that overrides BeginPlay without calling Super bypasses
 * hook 1 silently, and that is not checkable from the SML stub tree. Hook 2 catches those, and it costs
 * one integer compare (`IsValidIndex(0)`, FORCEINLINE at :229) on a path that is about to do far more
 * work than that.
 *
 * ⚠ SIDE IS `Any`, DELIBERATELY CHANGED FROM THE OLD MOD'S CLIENT-ONLY. A zero-slot inventory on the
 * server is just as wrong, and an assert there kills EVERY player's session rather than one. Gating
 * this off the server would produce the contract's worst failure shape: doing nothing while the log
 * still says the mod loaded.
 */
class FFPMInventoryInitGuard final : public IFPMFix
{
public:
	static FFPMInventoryInitGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("inventory-init"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }
	virtual void Arm() override;
};
