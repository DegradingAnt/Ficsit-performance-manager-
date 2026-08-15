// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FGSaveInterface.h"
#include "Wrist/FPMWristItemInterface.h"
#include "FPMWristItemBase.generated.h"

class AFGCharacterPlayer;

/**
 * ★★ THE OPTIONAL CONVENIENCE BASE — and the class that carries the persistence design.
 *
 * [RULED — Ruling 21, `rulings:364-381`, her words verbatim: *"it needs to persist, like any other
 * item in the world."*] design §11.2.3 restates this as design law, and names zero-residue as NOT
 * reaching here: zero residue governs FPM state ABOUT THE MACHINE (`GameUserSettings.ini`, cvars);
 * a worn wrist item is world content the player owns, the same as every other content mod's item.
 * "Do not cite zero-residue against this design" — design §11.2.3.
 *
 * Third parties may subclass this for persistence "for free", or implement `IFPMWristItem` raw and
 * make their own `ShouldSave` choice — design §11.2.3 point 1: "the API docs state that persistence
 * is the expected contract," not an enforced one (audit §7, question 2 — open for Ant; the design's
 * own default is documented-but-unenforced, carried here unchanged).
 *
 * ═══ WHY `IFGSaveInterface`, receipts from the game's own header ═══
 *
 * [MEASURED — every citation below is a direct read of
 * `C:/Modding/SatisfactoryModLoader/Source/FactoryGame/Public/FGSaveInterface.h` this task.]
 * The interface's own doc comment: *"If you want your actor to be saved, implement this! This will
 * make an actor be detected by the save system and have it's SaveGame properties saved."*
 * (`FGSaveInterface.h:45-48`). Vanilla equipment already persists through the same interface —
 * `AFGEquipment : public AActor, public IFGSaveInterface` (`FGEquipment.h:186`) — so this is the
 * ordinary route for a runtime-spawned actor, not a special case.
 *
 * ═══ WHY THE ITEM PERSISTS AND NOT THE SLOT COMPONENT ═══
 *
 * The five vanilla equipment slots are plain `UPROPERTY(SaveGame)` MEMBERS on
 * `AFGCharacterPlayer` (`FGCharacterPlayer.h:2045-2063`, read directly this task). That route is
 * closed to `UFPMWristSlotComponent`, which is added dynamically at runtime
 * (`Public/Wrist/FPMWristSlotComponent.h`) and never enters that member list — a dynamically-added
 * component is not itself part of any save array by that route. So the ITEM carries the
 * persistence, and the component re-associates with it on load through the handshake below.
 *
 * ═══ THE PERSISTENCE HANDSHAKE (design §11.2.3, point 3) ═══
 *
 * The load order between this save-spawned actor and the hook-added slot component is NOT
 * guaranteed. `PostLoadGame_Implementation` registers this item into
 * `UFPMWristSlotComponent`'s transient pending-worn map, keyed on `mOwnerCharacter` (itself a saved
 * actor reference, restored by the time `PostLoadGame` runs — see `FGSaveInterface.h:93` and the
 * ordering guarantee `GatherDependencies` exists to uphold). Whichever side of the handshake arrives
 * second — this registration, or the component's own add-hook querying the map for its character —
 * completes the equip, server-side, no RCO, no config read, no schematic re-validation: **the save
 * is the authority on what is worn, exactly as it is for every other item.**
 *
 * ═══ THE UNINSTALL CASE (design §11.2.3, answered in the design, not deferred) ═══
 *
 * By construction the reference direction is item -> character only: this actor is never placed in
 * a vanilla inventory, equipment slot, or saved container. So if FPM is removed, the save loader
 * skips the record for the unloadable class and no vanilla object is left holding a null reference
 * to it — the failure shape is structurally excluded, not merely hoped against. Boot row **B22**
 * verifies this on a real save; nothing here waits on that boot to ship (design 3158: default is
 * ship as designed).
 */
UCLASS( Abstract, Blueprintable )
class FICSITSPERFORMANCEMANAGER_API AFPMWristItemBase : public AActor, public IFPMWristItem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	AFPMWristItemBase();

	//~Begin IFGSaveInterface
	virtual bool ShouldSave_Implementation() const override { return true; }
	/**
	 * The item rides its owner's attach socket every frame — a stored world transform would be
	 * stale the instant it is loaded. design §11.2.3, point 1.
	 */
	virtual bool NeedTransform_Implementation() override { return false; }
	virtual void PostLoadGame_Implementation( int32 SaveVersion, int32 GameVersion ) override;
	//~End IFGSaveInterface

	//~Begin IFPMWristItem
	virtual FName GetWristItemId_Implementation() const override { return mWristItemId; }
	virtual FText GetWristItemDisplayName_Implementation() const override { return mDisplayName; }
	virtual void WristEquip_Implementation( AFGCharacterPlayer* NewOwner ) override;
	virtual void WristUnequip_Implementation() override;
	virtual bool WristDeploy_Implementation() override;
	virtual void WristRelease_Implementation() override;
	virtual FName GetWristAttachSocket_Implementation( bool bRightHanded ) const override;
	//~End IFPMWristItem

protected:
	/**
	 * The owning character — the ONE thing this base saves beyond the engine's own actor bookkeeping.
	 * A `SaveGame` reference to another saved actor restores across load like any other cross-actor
	 * save reference (design §11.2.3, point 2; the same shape `FPMWireNullGuard` already cleans when
	 * IT goes stale — power wires referencing removed poles).
	 */
	UPROPERTY( SaveGame, BlueprintReadOnly, Category = "FPM|Wrist" )
	TObjectPtr<AFGCharacterPlayer> mOwnerCharacter;

	/** Registry key half — set by the subclass or by data. Not SaveGame: it is authored, not runtime state. */
	UPROPERTY( EditDefaultsOnly, BlueprintReadOnly, Category = "FPM|Wrist" )
	FName mWristItemId;

	UPROPERTY( EditDefaultsOnly, BlueprintReadOnly, Category = "FPM|Wrist" )
	FText mDisplayName;

	/**
	 * Data on the item's own registration, never a hardcoded name at a callsite — Ruling 11.
	 * B11 (`FPM2-DESIGN-ASSEMBLED.md:3147`) has not measured the real vanilla socket names; these are
	 * the stated DEFAULT until then (hand grip sockets, mirrored) and are placeholders, not verified
	 * names — no model/animation work exists yet either (design §11.2.2's editor-asset cost, deferred).
	 */
	UPROPERTY( EditDefaultsOnly, BlueprintReadOnly, Category = "FPM|Wrist" )
	FName mSocketLeft = TEXT( "hand_lSocket" );

	UPROPERTY( EditDefaultsOnly, BlueprintReadOnly, Category = "FPM|Wrist" )
	FName mSocketRight = TEXT( "hand_rSocket" );
};
