// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "FPMWristItemInterface.generated.h"

/**
 * ★ THE CONTRACT EVERY WRIST ITEM IMPLEMENTS — OURS AND THIRD-PARTY ALIKE (design §11.2.2, §11.2.6).
 *
 * [RULED — Ruling 14, "hookable by other mods if they wish"; Ruling 11, attachment parameterised by
 * side from the start.] This is the one PUBLIC surface a mod registers against
 * (`UFPMWristSlotComponent::RegisterWristItem`, `Public/Wrist/FPMWristSlotComponent.h`). Anything
 * else inside FPM is unsupported per §11.2.6's own closing line.
 *
 * BlueprintNativeEvent throughout, so a third-party item may be pure Blueprint, pure C++, or a
 * subclass of the optional convenience base (`AFPMWristItemBase`) that already answers most of
 * these. `Blueprintable` on the UINTERFACE for the same reason.
 *
 * ⚠ THESE ARE LOCAL/PRESENTATION ENTRY POINTS, NOT THE NETWORK BOUNDARY. `UFPMWristSlotComponent`
 * owns the server RPCs (`Server_Equip` etc., design 11.2.5's failure-path table: "RCO — never
 * multicast in an RCO"); it calls these on the resolved item ACTOR after authority has already
 * decided the action is allowed. An item implementing this interface never needs its own RPCs for
 * the base equip/deploy lifecycle.
 */
UINTERFACE( Blueprintable )
class FICSITSPERFORMANCEMANAGER_API UFPMWristItem : public UInterface
{
	GENERATED_BODY()
};

class FICSITSPERFORMANCEMANAGER_API IFPMWristItem
{
	GENERATED_BODY()

public:
	/** Stable id for this item WITHIN its owning mod — the registry key is (OwnerModReference, ItemId). */
	UFUNCTION( BlueprintNativeEvent, Category = "FPM|Wrist" )
	FName GetWristItemId() const;

	/** Shown in whatever UI a mod builds for slot selection. Not FPM's job to render — §11.2.6. */
	UFUNCTION( BlueprintNativeEvent, Category = "FPM|Wrist" )
	FText GetWristItemDisplayName() const;

	/**
	 * The slot has decided this item is now worn by `NewOwner`. Called server-side only, after
	 * `UFPMWristSlotComponent::Server_Equip` has already checked `SlotOccupied` — an item's own
	 * `Equip` implementation is bookkeeping (attach, remember the owner), never a second gate.
	 */
	UFUNCTION( BlueprintNativeEvent, Category = "FPM|Wrist" )
	void WristEquip( class AFGCharacterPlayer* NewOwner );

	/** The slot is releasing this item — the component is holstering or swapping to another item. */
	UFUNCTION( BlueprintNativeEvent, Category = "FPM|Wrist" )
	void WristUnequip();

	/**
	 * Move from holstered to deployed (design §11.2.2 — "equipped = armed, always-on... fire/release
	 * are input actions"). Returns false to refuse (e.g. on cooldown); the component does not
	 * second-guess a refusal.
	 */
	UFUNCTION( BlueprintNativeEvent, Category = "FPM|Wrist" )
	bool WristDeploy();

	/** Move from deployed back to holstered. Always succeeds — release cannot be refused. */
	UFUNCTION( BlueprintNativeEvent, Category = "FPM|Wrist" )
	void WristRelease();

	/**
	 * The attach socket name for the given side, resolved AT ATTACH TIME per the handedness setting
	 * [RULED — Ruling 11: "the retrofit is the expensive failure"]. `SocketLeft`/`SocketRight` are
	 * data on the item's own registration, never a hardcoded name at a callsite.
	 *
	 * Boot question B11 (`FPM2-DESIGN-ASSEMBLED.md:3147`) enumerates the vanilla skeleton's real
	 * socket names. DEFAULT until measured: the hand grip sockets, mirrored — this cannot be verified
	 * offline, so implementers should treat the returned name as provisional until B11 lands.
	 */
	UFUNCTION( BlueprintNativeEvent, Category = "FPM|Wrist" )
	FName GetWristAttachSocket( bool bRightHanded ) const;
};
