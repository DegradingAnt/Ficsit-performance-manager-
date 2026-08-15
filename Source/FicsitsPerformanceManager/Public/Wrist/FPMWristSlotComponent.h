// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/FPMFixContract.h"
#include "Wrist/FPMWristItemInterface.h"
#include "FPMWristSlotComponent.generated.h"

class AFGCharacterPlayer;
class AFPMWristItemBase;

/**
 * ★ THE VERSION CONSTANTS — design §11.2.6's `{Major, Minor}` model, `Ruling 14`.
 *
 * A registering mod passes the pair it compiled against. Registration is ACCEPTED when
 * `consumer.Major == host.Major && consumer.Minor <= host.Minor` — see
 * `UFPMWristSlotComponent::RegisterWristItem`. Bump Minor for an additive change (new interface
 * member, new registration field); bump Major only for a breaking one, after the deprecation path
 * design §11.2.6 describes (kept working two public releases, one log line per session while
 * deprecated, removed only at the Major bump).
 */
inline constexpr int32 FPM_WRIST_API_MAJOR = 1;
inline constexpr int32 FPM_WRIST_API_MINOR = 0;

/** Typed refusals from design §11.2.5's failure-path table, named so a caller can branch on WHY. */
UENUM( BlueprintType )
enum class EFPMWristRefusal : uint8
{
	None,
	/** Another wrist item is already equipped. The UI offers a swap; nothing auto-unequips. */
	SlotOccupied,
	/** The wrist feature or the master switch is OFF (design §11.2.6's OFF contract). */
	SlotDisabled,
	/** `RegisterWristItem`'s `{Major,Minor}` check failed. Refused at registration, never at equip. */
	VersionMismatch,
	/** The actor passed to `Server_Equip` does not implement `IFPMWristItem`. */
	NotAWristItem,
};

/** Fired on every character's own component when the slot's ENABLED/DISABLED state changes. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam( FFPMWristSlotStateChangedDelegate, bool, bEnabled );

/**
 * ★★ THE WRIST SLOT — a new, FPM-owned equipment channel, parallel to and outside the game's five
 * `EEquipmentSlot` values (design §11.2.2; `Ruling 113-114`: *"we should make another equipment
 * slot and put this in it... should be hoockable by other mods if they wish"*).
 *
 * ⚠ WHY NOT `EEquipmentSlot::ES_WRIST`. `ES_MAX` bounds a COMPILED enum
 * (`FGEquipment.h:38-47`) — a mod cannot add an enumerator to it. This component is FPM's own
 * parallel channel instead (route 2 of the three the ruling weighed; route 1, squatting on
 * `ES_LEGS`/`ES_HEAD`/`ES_BODY`, gives other mods nothing to hook — the opposite of the spec).
 *
 * ONE INSTANCE PER `AFGCharacterPlayer`, added server-side by `FFPMWristSlotHook` (this file, below)
 * via a `SUBSCRIBE_METHOD_VIRTUAL_AFTER` hook on `AFGCharacterPlayer::BeginPlay`, guarded on
 * `HasAuthority()`. It owns the equipped item, the deployed/holstered state, and the handedness —
 * standard UE component replication delivers all three to clients, no bespoke channel (§5.5's table).
 *
 * ★★★ THE PUBLIC API LIVES ON THIS CLASS, NOT ON A SEPARATE SUBSYSTEM. JUDGEMENT CALL, stated here
 * because it is load-bearing for the reflected-surface budget Ruling 14 priced: design §11.2.2 says
 * the wrist system costs "2 UCLASS + 1 UINTERFACE" against a measured baseline of 3 UCLASS / 0
 * USTRUCT / 0 UINTERFACE, and says Ruling 14 "prices and accepts EXACTLY this." A third UCLASS for
 * a `UGameInstanceSubsystem`/`UWorldSubsystem` registry would spend budget nobody approved. So the
 * registration API (`RegisterWristItem`, `GetWristApiVersion`, `IsWristFeatureEnabled`) ships as
 * STATIC `UFUNCTION`s on THIS class instead — reflection-reachable exactly the way a Blueprint
 * Function Library's statics are, and callable from the optional-dependency header (§5.10) the same
 * way. The registry storage itself is plain file-scope state in the .cpp (mirroring
 * `FPMHookLedger`'s own static-module-state shape), not a second UCLASS either. `FFPMWristItemDesc`
 * is deliberately NOT a `USTRUCT` for the same reason — its fields are passed as separate UFUNCTION
 * parameters so the measured 0-USTRUCT baseline stays 0.
 *
 * PERSISTENCE (design §11.2.3, `Ruling 21`: *"it needs to persist, like any other item in the
 * world"*) is NOT this component's job — the vanilla `SaveGame` slot route is closed to a
 * dynamically-added component (`FGCharacterPlayer.h:2045-2063` are plain member `UPROPERTY(
 * SaveGame)`s; a component added at runtime through `NewObject`+`RegisterComponent` never enters
 * that array). The ITEM persists instead, through `AFPMWristItemBase : IFGSaveInterface`
 * (`Public/Wrist/FPMWristItemBase.h`). This class supplies the OTHER half of the load-order
 * handshake design §11.2.3 describes: a transient, unsaved pending-worn map, because the load order
 * between the save-spawned item actor and this hook-added component is not guaranteed.
 */
UCLASS( ClassGroup = ( FPM ), meta = ( BlueprintSpawnableComponent ) )
class FICSITSPERFORMANCEMANAGER_API UFPMWristSlotComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFPMWristSlotComponent();

	// ════════════════════════════════════════════════════════════════════════════════════════════
	// THE PUBLIC WRIST-SLOT API (design §11.2.6, `Ruling 14`) — static, see the class comment above
	// for why it lives here instead of on a separate subsystem class.
	// ════════════════════════════════════════════════════════════════════════════════════════════

	/**
	 * A third-party mod's registration call. Duplicate `{OwnerModReference, ItemId}` replaces its own
	 * entry; two DIFFERENT owners never collide because the owner is part of the key. Registration is
	 * a CATALOG entry, not an equip — any number of mods may register; equipping stays exclusive.
	 *
	 * Refused, loudly, with `EFPMWristRefusal::VersionMismatch`, when
	 * `!(ConsumerMajor == FPM_WRIST_API_MAJOR && ConsumerMinor <= FPM_WRIST_API_MINOR)` — one log line
	 * naming both pairs, per design §11.2.6, never discovered later as a crash.
	 *
	 * @return true on success. False means refused; check the log for the reason (also returned via
	 *         OutRefusal for a caller that wants to branch on it without parsing a log line).
	 */
	UFUNCTION( BlueprintCallable, Category = "FPM|Wrist" )
	static bool RegisterWristItem( FName OwnerModReference, FName ItemId, TSoftClassPtr<AActor> ItemClass,
		const FText& DisplayName, int32 ConsumerMajor, int32 ConsumerMinor,
		EFPMWristRefusal& OutRefusal );

	/** `{FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR}`, for a caller that wants it without the header. */
	UFUNCTION( BlueprintPure, Category = "FPM|Wrist" )
	static void GetWristApiVersion( int32& OutMajor, int32& OutMinor );

	/**
	 * True when the wrist feature AND the master switch both allow action. Design §11.2.6's OFF
	 * contract: registration still succeeds while this is false — only `Equip`/`Deploy` decline.
	 */
	UFUNCTION( BlueprintPure, Category = "FPM|Wrist" )
	static bool IsWristFeatureEnabled();

	/**
	 * How many wrist items are currently registered, across every owning mod. Diagnostic surface —
	 * `FPM.Wrist.Report` (the .cpp) prints the full catalog; this is the number for a caller that only
	 * wants the count.
	 */
	UFUNCTION( BlueprintPure, Category = "FPM|Wrist" )
	static int32 GetRegisteredWristItemCount();

	// ════════════════════════════════════════════════════════════════════════════════════════════
	// THE PERSISTENCE HANDSHAKE (design §11.2.3) — the pending-worn map's public face.
	// ════════════════════════════════════════════════════════════════════════════════════════════

	/**
	 * Called from `AFPMWristItemBase::PostLoadGame_Implementation`. The item's `mOwnerCharacter` has
	 * already been restored by the save system (it is a `UPROPERTY(SaveGame)` reference to another
	 * saved actor) — this registers the item under that character so whichever side of the handshake
	 * arrives second (this call, or the component's own add-hook) can complete the equip.
	 */
	static void RegisterPendingWorn( AFGCharacterPlayer* Character, AFPMWristItemBase* Item );

	/**
	 * Called from the component's add-hook once the component exists for `Character`. Returns and
	 * REMOVES the pending item if one was registered for this character, or nullptr if the item side
	 * of the handshake has not arrived yet (load order is not guaranteed either way).
	 */
	static AFPMWristItemBase* ClaimPendingWorn( AFGCharacterPlayer* Character );

	// ════════════════════════════════════════════════════════════════════════════════════════════
	// PER-CHARACTER STATE — replicated, standard UE component replication (design §11.2.2/§5.5).
	// ════════════════════════════════════════════════════════════════════════════════════════════

	UFUNCTION( BlueprintPure, Category = "FPM|Wrist" )
	AActor* GetEquippedItemActor() const { return mEquippedItemActor; }

	/** Casts the equipped actor to the interface. nullptr when nothing is equipped. */
	IFPMWristItem* GetEquippedInterface() const;

	UFUNCTION( BlueprintPure, Category = "FPM|Wrist" )
	bool IsDeployed() const { return bDeployed; }

	UFUNCTION( BlueprintPure, Category = "FPM|Wrist" )
	bool IsRightHanded() const { return bRightHanded; }

	/** Fires locally on THIS character's component when the feature toggles. §11.2.6's contract. */
	UPROPERTY( BlueprintAssignable, Category = "FPM|Wrist" )
	FFPMWristSlotStateChangedDelegate OnWristSlotStateChanged;

	// ════════════════════════════════════════════════════════════════════════════════════════════
	// CLIENT-TO-SERVER ACTIONS — design §11.2.5's table: "RCO — never multicast in an RCO, at least
	// one replicated UPROPERTY or it silently fails." Every one of these mutates a replicated
	// UPROPERTY above (`mEquippedItemActor` / `bDeployed` / `bRightHanded`) on success.
	// ════════════════════════════════════════════════════════════════════════════════════════════

	/** `ItemActor` must implement `IFPMWristItem`. Refuses `SlotOccupied` / `SlotDisabled` / `NotAWristItem`. */
	UFUNCTION( Server, Reliable, Category = "FPM|Wrist" )
	void Server_Equip( AActor* ItemActor );

	/** Releases first if deployed (design §11.2.2's failure-path table, "one order, always"), then unequips. */
	UFUNCTION( Server, Reliable, Category = "FPM|Wrist" )
	void Server_Unequip();

	/** Holstered -> deployed. Refuses `SlotDisabled` / no item equipped, or the item's own `WristDeploy` refusal. */
	UFUNCTION( Server, Reliable, Category = "FPM|Wrist" )
	void Server_Deploy();

	/** Deployed -> holstered. Cannot be refused by the item (interface contract) once it is called. */
	UFUNCTION( Server, Reliable, Category = "FPM|Wrist" )
	void Server_Release();

	/**
	 * The owning client pushes ITS OWN local `FPM.Wrist.RightHanded` preference. Bound from
	 * `FFPMWristSlotHook`'s `OnPlayerInputInitialized` handler on the locally-controlled character —
	 * see that class's `Arm()`. A deployed item keeps its current side until release (design
	 * §11.2.2's "no mid-flight re-attach"), enforced here by ignoring the change while `bDeployed`.
	 */
	UFUNCTION( Server, Reliable, Category = "FPM|Wrist" )
	void Server_SetHandedness( bool bNewRightHanded );

	/** Server -> the acting client only, so its UI can show WHY an action did nothing. */
	UFUNCTION( Client, Reliable, Category = "FPM|Wrist" )
	void Client_OnActionRefused( FName Action, EFPMWristRefusal Reason );

	/**
	 * ★ THE OFF CONTRACT (design §11.2.6): "toggling OFF force-releases any deployed rope and
	 * unequips to holstered state, then declines further action." Called on the AUTHORITY only, from
	 * `FFPMWristSlotHook`'s master-switch stop-hook and from the `FPM.Wrist.Enabled` cvar sink — see
	 * that class's `Arm()`. Idempotent: a no-op on an already-holstered component.
	 */
	void ForceHolster( const TCHAR* Reason );

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type EndPlayReason ) override;
	virtual void GetLifetimeReplicatedProps( TArray<FLifetimeProperty>& OutLifetimeProps ) const override;

private:
	/**
	 * The equipped item, stored as the ACTOR rather than `TScriptInterface<IFPMWristItem>` — an
	 * interface pointer does not replicate cleanly; the actor does, and `GetEquippedInterface()`
	 * casts on demand. nullptr when nothing is equipped.
	 */
	UPROPERTY( Replicated )
	TObjectPtr<AActor> mEquippedItemActor;

	UPROPERTY( Replicated )
	bool bDeployed = false;

	/** LEFT default per §11.2.2's ruling; RIGHT by the player's own `FPM.Wrist.RightHanded` choice. */
	UPROPERTY( ReplicatedUsing = OnRep_Handedness )
	bool bRightHanded = false;

	UFUNCTION()
	void OnRep_Handedness();
};

/**
 * ★ THE ADD-HOOK, wrapped as an `IFPMFix` for consistency with every other native hook in this mod
 * (23/23 files that call `FPM_SUBSCRIBE_VIRTUAL*` do so from inside an `IFPMFix::Arm()` — checked by
 * grep before writing this). That gets the wrist slot DisarmAll/RearmAll participation, a
 * `FPM.Fix.WristSlot` toggle, and a crash-stamp roster entry, for free.
 *
 * ⚠ `OriginStatus()` IS AN IMPERFECT FIT, STATED RATHER THAN HIDDEN. `EFPMOriginStatus`'s four values
 * (`FPMFixContract.h`) are all about a DEFECT-claim's evidence tier — none of them describes a hook
 * that PROVISIONS a new capability rather than fixing one. `Guard` is chosen as the least-wrong
 * available value (this hook's job is structural/defensive: guarantee the component invariant
 * "every `AFGCharacterPlayer` has a slot", never a claim that something was broken and is now fixed)
 * — but this is a genuine judgement call, not a confident classification, and the contract may want
 * a fifth value for feature-provisioning hooks. Flagged for review rather than silently picked.
 */
class FFPMWristSlotHook final : public IFPMFix
{
public:
	static FFPMWristSlotHook& Get();

	virtual const TCHAR* Name() const override { return TEXT("wrist-slot"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }
	virtual FPMDiag::EChannel Channel() const override;
	virtual void Arm() override;
	virtual void Disarm() override;

private:
	/** From the `BeginPlay` AFTER-hook that creates and registers the component. */
	FDelegateHandle BeginPlayHandle;
	/** From `AFGCharacterPlayer::OnPlayerInputInitialized` — a plain (non-ledger) multicast delegate;
	 *  see the .cpp for why it is bound here rather than through `FPMHookLedger`. */
	FDelegateHandle InputInitHandle;
};
