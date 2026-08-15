// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Wrist/FPMWristItemBase.h"
#include "Core/FPMFixContract.h"
#include "FPMTowItem.generated.h"

class USkeletalMeshComponent;
class USkeletalMesh;
class UAnimSequence;

/**
 * The hook has two shapes and one transition between them.
 *
 * [RULED 2026-08-15, savestate "THE HOOK SNAPS OPEN"] The hook is a slim dart in flight. The flukes
 * deploy on impact. A splayed hook looks wrong stowed and wrong in flight, but splayed is the only
 * shape that holds a load. Snap-open makes the hook slim when it must be slim, and wide only when it
 * bites.
 *
 * `Deploying` and `Stowing` are the transition. They exist because the transition has a DURATION when
 * a deploy AnimSequence is present, and because the moment of attach is the moment the player looks
 * at the device. When no AnimSequence is present, the item passes through the transition state inside
 * the same call and lands on the end state. A caller therefore never has to special-case the missing
 * asset.
 */
UENUM( BlueprintType )
enum class EFPMTowHookState : uint8
{
	Stowed,
	Deploying,
	Deployed,
	Stowing
};

/**
 * How one mount point name resolved against the skeletal mesh. Three values, not a bool, because the
 * two positive values have DIFFERENT art-side causes and different fixes.
 *
 * ⚠ MEASURED THIS TASK, and it is the trap the static-mesh version of this file did not have.
 * `USkinnedMeshComponent::DoesSocketExist` returns TRUE FOR A BONE NAME as well as for a socket.
 * `SkinnedMeshComponent.cpp:3405` returns `GetSocketBoneName(...) != NAME_None`, and
 * `GetSocketBoneName` (`:3410-3435`) first calls `FindSocket`, then falls back to `GetBoneIndex` and
 * returns the name itself when the name is a bone. `GetSocketByName` does NOT do that. It reaches
 * `GetSocketInfoByName` (`:3287-3314`), which only calls `FindSocketInfo` on the asset, so it returns
 * null for a bone. `GetSocketTransform` (`:3221-3262`) resolves BOTH: socket branch first, bone
 * branch second.
 *
 * So a `DoesSocketExist` result is not proof of a socket, and a null `GetSocketByName` is not proof
 * of nothing. This enum is the reason the report can never state one as the other.
 */
UENUM( BlueprintType )
enum class EFPMTowMountResolution : uint8
{
	/** Neither a socket nor a bone carries this name. Nothing can attach here. */
	NotFound,
	/** A mesh socket or a skeleton socket carries this name. This is the authored intent. */
	Socket,
	/** A BONE carries this name and no socket does. Usable, but the art did not author a socket. */
	Bone
};

/**
 * ★ TOW (working name, Ant's ruling in tow-name.md: "Transit Overhead Winch", status WORK IN
 * PROGRESS). THE FIRST CONCRETE WRIST ITEM, AND IT SHIPS INERT.
 *
 * Follows `AFPMWristItemBase` exactly rather than inventing a second pattern. That base already
 * attaches THIS ACTOR to the CHARACTER's hand socket inside `WristEquip_Implementation`
 * (`FPMWristItemBase.cpp:68`, `AttachToComponent(Mesh, SnapToTargetIncludingScale, Socket)`). This
 * class supplies the device's OWN root component and the DEVICE-LOCAL mount points. That is a
 * different question from the one the base already answers (`mSocketLeft` and `mSocketRight`: where
 * on the CHARACTER this item attaches).
 *
 * ═══ WHY THIS IS A SKELETAL MESH ═══
 *
 * [RULED 2026-08-15] The hook SNAPS OPEN. That is one skeletal mesh with two poses, not a static
 * mesh. Her own words in the savestate RESUME HERE list say this is cheaper now and expensive to
 * retrofit, so the mesh component type is decided before anything is wired on top of it.
 *
 * The two poses are driven FROM CODE, not from an AnimBlueprint asset:
 * `USkeletalMeshComponent::SetAnimationMode(EAnimationMode::AnimationSingleNode)` plus
 * `SetAnimation`, `SetPosition`, `SetPlayRate` and `Play`. Ant is not fluent in the editor yet, so an
 * asset route costs far more of her time than a code route.
 *
 * Reverse playback is a NEGATIVE PLAY RATE. `FAnimSingleNodeInstanceProxy::SetReverse`
 * (`AnimSingleNodeInstanceProxy.cpp:246-256`) does exactly `PlayRate = -FMath::Abs(PlayRate)`, so
 * this class sets the negative rate itself and keeps ONE code path for both directions.
 *
 * ⚠ The engine's own doc comment on `SetAnimation`, `Play`, `SetPosition` and `SetPlayRate`
 * (`SkeletalMeshComponent.h:1160-1231`) states that they change TRANSIENT instance data and that
 * they are "not safe to be used during construction script". This class calls none of them from the
 * constructor. The first call is in `BeginPlay`.
 *
 * ═══ THE THREE DEVICE MOUNT POINTS, AND THE IMPORT QUESTION THEY DEPEND ON ═══
 *
 * `ArtSource/TOW/TOW_placeholder_v1.glb` carries three empty (mesh-less) nodes: `SOCKET_Mount`,
 * `SOCKET_LineExit` and `SOCKET_HookStow`.
 *
 * ⚠ CORRECTED THIS TASK. The earlier version of this comment said that Interchange strips a
 * `SOCKET_` prefix and makes a socket from the remainder. That is the STATIC MESH path. On a
 * SKELETAL mesh the three mount points must be authored as SKELETON SOCKETS or as BONES. The art
 * contract file (`ArtSource/TOW/TOW_ART_CONTRACT.md`) states this to the art side, and
 * `ClassifyMountPoint` below reports which of the two actually landed instead of assuming either.
 *
 * ⚠ MEASURED THIS TASK, out of the glb's own JSON chunk (289236 bytes, glTF binary v2): 26 meshes,
 * 26 mesh-bearing nodes, 3 mesh-less socket nodes, 29 scene roots, ZERO nodes with children, ZERO
 * skins and ZERO animations. A figure of "about 111 mesh parts" circulated in an earlier version of
 * this comment. It is wrong: the count is 26.
 *
 * The zero skins figure is the one that decides this file's fate. A glTF with no skin has no
 * skeleton, so THE CURRENT PLACEHOLDER CANNOT IMPORT AS A SKELETAL MESH AT ALL, and its three flat
 * socket nodes would be dropped even if it could. Both are art-side fixes, not code fixes. This class
 * reports the state it finds rather than assuming either fix already happened. See
 * `ArtSource/TOW/TOW_ART_CONTRACT.md`.
 *
 * ═══ SHIPS INERT, SAME SHAPE AS FFPMThirdPersonToggle, SAME REASON ═══
 *
 * NO MESH ASSET EXISTS YET. `mDeviceMeshAsset` and `mDeployAnimAsset` are `TSoftObjectPtr`, and
 * NEITHER is resolved in the constructor. `ConstructorHelpers::FObjectFinder` would eager-load at
 * CLASS-DEFAULT-OBJECT construction time. That happens automatically the first time reflection
 * touches this UCLASS, and it is NOT gated by `IFPMFix::DefaultArmed()` at all, which only gates
 * whether `Arm()` runs. A soft pointer assigned by a plain `FSoftObjectPath` never triggers a load
 * until something explicitly resolves it, so this class's CDO construction has zero cost and zero log
 * spam regardless of arm state.
 *
 * `FFPMTowItemHook` (below) registers this class into the wrist catalog. It ships
 * `DefaultArmed() == false` for the same reason `FFPMThirdPersonToggle` does.
 */
UCLASS( Blueprintable )
class FICSITSPERFORMANCEMANAGER_API AFPMTowItem : public AFPMWristItemBase
{
	GENERATED_BODY()

public:
	AFPMTowItem();

	/**
	 * ★ THE THREE MOUNT POINT NAMES, DECLARED EXACTLY ONCE IN THE WHOLE MOD.
	 *
	 * `ArtSource/TOW/TOW_ART_CONTRACT.md` states these same three strings to the art side. Nothing in
	 * the .cpp holds a second copy. Every log line and every report prints these members, so the
	 * contract file and the code cannot drift apart without the drift being visible in one place.
	 *
	 * A name may land as a socket or as a bone. See `EFPMTowMountResolution`.
	 */
	static const FName MountName;
	static const FName LineExitName;
	static const FName HookStowName;

	//~Begin AActor
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps( TArray<FLifetimeProperty>& OutLifetimeProps ) const override;
	//~End AActor

	//~Begin IFPMWristItem
	/** Snaps the hook open. See `EFPMTowHookState`. Never refuses: this item has no cooldown yet. */
	virtual bool WristDeploy_Implementation() override;
	/** Snaps the hook closed, by playing the same AnimSequence at a negative play rate. */
	virtual void WristRelease_Implementation() override;
	//~End IFPMWristItem

	UFUNCTION( BlueprintPure, Category = "FPM|Wrist|TOW" )
	EFPMTowHookState GetHookState() const { return mHookState; }

	/**
	 * ★ THE ONE CLASSIFIER. Static and pure, so the report can exercise it against a known-positive
	 * and a known-negative without an actor, a world, or an equipped item.
	 *
	 * Returns `Socket` when `USkeletalMesh::FindSocket` finds the name. That call searches the mesh's
	 * own socket list AND the skeleton's (`SkeletalMesh.cpp:4759-4799`), so a skeleton socket counts.
	 * Returns `Bone` when no socket carries the name but the reference skeleton does. Returns
	 * `NotFound` for a null mesh or an unknown name.
	 *
	 * The runtime component path agrees with this by construction, not by hope.
	 * `USkinnedMeshComponent::GetSocketByName` and `GetBoneIndex` both delegate to this same asset,
	 * and this class never sets a socket override, which is the only thing that could make them
	 * differ (`SkinnedMeshComponent.cpp:3289-3298`).
	 */
	static EFPMTowMountResolution ClassifyMountPoint( const USkeletalMesh* Mesh, FName MountPointName );

	/**
	 * ★ WHAT THE ROPE LANE NEEDS FROM THIS MOUNT POINT, ANSWERED DIRECTLY. The rope lane's own
	 * architecture doc (lane-e2-rope-architecture.md) feeds
	 * `UInstancedStaticMeshComponent::BatchUpdateInstancesTransforms` a CURRENT and a PREVIOUS
	 * world-space position for its near (player-held) segment every frame. This is that position,
	 * resolved LIVE and NEVER CACHED. The item rides the character's socket every frame
	 * (`FPMWristItemBase.h`'s own `NeedTransform_Implementation` comment: a stored transform would be
	 * stale the instant the arm moves), so the rope lane must call this once per tick that it needs a
	 * fresh anchor.
	 *
	 * Returns false, and leaves `OutTransform` at identity, when the mesh or the mount point did not
	 * resolve. THE ROPE LANE MUST TREAT A FALSE RETURN AS "NO ANCHOR AVAILABLE". Never substitute a
	 * guessed offset from the character's hand socket, which is a different, unrelated transform.
	 *
	 * A mount point that resolved as a BONE returns true. `GetSocketTransform` resolves a bone name as
	 * well as a socket name (`SkinnedMeshComponent.cpp:3247-3253`), so the transform is real. The
	 * report says WHICH of the two it was, because that is an art-quality question, not a runtime one.
	 */
	UFUNCTION( BlueprintCallable, Category = "FPM|Wrist|TOW" )
	bool GetLineExitTransform( FTransform& OutTransform ) const;

	/** Same contract as `GetLineExitTransform`, for the device's own mount point. */
	UFUNCTION( BlueprintCallable, Category = "FPM|Wrist|TOW" )
	bool GetMountTransform( FTransform& OutTransform ) const;

	/** Same contract as `GetLineExitTransform`, for where the hook parks when stowed. */
	UFUNCTION( BlueprintCallable, Category = "FPM|Wrist|TOW" )
	bool GetHookStowTransform( FTransform& OutTransform ) const;

protected:
	/**
	 * The device's own render mesh AND this actor's root component. The base's attach call
	 * (`FPMWristItemBase.cpp:69`) snaps THIS component to the character's hand socket.
	 *
	 * SKELETAL, not static: the hook snaps open, which is two poses of one mesh.
	 */
	UPROPERTY( VisibleAnywhere, BlueprintReadOnly, Category = "FPM|Wrist|TOW" )
	TObjectPtr<USkeletalMeshComponent> mDeviceMesh;

	/**
	 * Soft reference only. See the class comment for why the constructor never resolves this.
	 * Nothing exists at this path today.
	 */
	UPROPERTY( EditDefaultsOnly, BlueprintReadOnly, Category = "FPM|Wrist|TOW" )
	TSoftObjectPtr<USkeletalMesh> mDeviceMeshAsset;

	/**
	 * The single deploy pose, played FORWARD to snap open and BACKWARD to snap closed. One asset and
	 * not two, so the closed pose can never drift from the open pose's own start frame.
	 *
	 * Soft reference only, for the same CDO reason as `mDeviceMeshAsset`. When this asset is absent
	 * the item still changes state. It changes it instantly. See `EFPMTowHookState`.
	 */
	UPROPERTY( EditDefaultsOnly, BlueprintReadOnly, Category = "FPM|Wrist|TOW" )
	TSoftObjectPtr<UAnimSequence> mDeployAnimAsset;

	/**
	 * Play rate for the snap, as a multiple of the AnimSequence's own authored speed. The stow
	 * direction uses the negative of this value. A value at or below zero is refused at use time and
	 * falls back to 1.0, because a zero rate would leave the hook stuck mid-snap forever.
	 */
	UPROPERTY( EditDefaultsOnly, BlueprintReadOnly, Category = "FPM|Wrist|TOW" )
	float mPoseRate = 1.0f;

	/**
	 * ⚠ REPLICATED, and that is a decision worth stating rather than leaving implicit.
	 * `UFPMWristSlotComponent` calls `WristDeploy` and `WristRelease` on the SERVER only
	 * (`FPMWristSlotComponent.cpp:791` and `:824`). Its own `bDeployed` replicates, but the item's
	 * `WristDeploy` never runs on a remote client, so without this property a remote player would see
	 * the hook stuck in one pose forever. `OnRep_HookState` applies the pose on the client.
	 *
	 * ⚠ NOT MEASURED. No mesh asset exists, so this has never been seen to move on a real client.
	 * It is designed, not proven.
	 *
	 * Not `SaveGame`. The hook state is presentation, and a save that restored a mid-snap state would
	 * be restoring a state that lasts a fraction of a second.
	 */
	UPROPERTY( ReplicatedUsing = OnRep_HookState, BlueprintReadOnly, Category = "FPM|Wrist|TOW" )
	EFPMTowHookState mHookState = EFPMTowHookState::Stowed;

	UFUNCTION()
	void OnRep_HookState();

private:
	/** Shared body of the three public getters. Accepts a bone as well as a socket. */
	bool ResolveMountPoint( FName MountPointName, FTransform& OutTransform ) const;

	/**
	 * Starts the pose in the requested direction and returns the seconds that the transition will
	 * take. Returns 0 when no AnimSequence resolved, which is the caller's signal to land on the end
	 * state inside the same call.
	 */
	float StartPose( bool bOpening );

	/**
	 * The whole transition, in one place: set the transition state, start the pose, and arm the one
	 * timer that ends it. Deploy and release differ only in the direction, so they share this.
	 */
	void BeginTransition( bool bOpening );

	/** Timer callback, AUTHORITY ONLY. Moves `Deploying` to `Deployed`, and `Stowing` to `Stowed`. */
	void FinishPose();

	/**
	 * Parks the pose on its first or last frame with no transition. This is the late-joiner path: a
	 * client that replicates straight into `Deployed` never saw the `Deploying` state, so it must not
	 * play the snap, it must already be open.
	 */
	void ApplyEndPose( bool bOpen );

	/** Loads the soft references. Called once from `BeginPlay`, never from the constructor. */
	void ResolveAssets();

	FTimerHandle mPoseTimer;

	/**
	 * `UAnimSequence::GetPlayLength()` at load time. Its own doc comment
	 * (`AnimSequenceBase.h:84-86`) says this is the total play length AT A SPEED OF 1.0, which is why
	 * the transition duration below divides it by the play rate rather than using it raw.
	 */
	float mPoseLengthSeconds = 0.0f;

	/** True once `ResolveAssets` has put the component into single-node animation mode. */
	bool bAnimReady = false;
};

/**
 * ★ THE CATALOG ENTRY, wrapped as an `IFPMFix` for the same reason `FFPMWristSlotHook` is:
 * consistency with every other native surface in this mod, `DisarmAll` and `RearmAll`
 * participation, an `FPM.Fix.TowItem` toggle, and a crash-stamp roster entry, all for free.
 *
 * SHIPS INERT. `DefaultArmed() == false`, same shape and same reason as `FFPMThirdPersonToggle`
 * (`Fixes/ModFeatures/FPMThirdPersonToggle.h`): its content half does not exist. Registering an item
 * whose mesh resolves to nothing would let a caller equip something that renders as nothing and
 * anchors a rope nowhere. That is worse than not registering it, because `SlotOccupied` would then
 * also refuse a REAL future item from taking the slot, for no visible benefit.
 *
 * FLIP `DefaultArmed()` TO true ONLY AFTER ALL THREE of these are independently true, in order:
 *   1. The art side authors the three mount points as SKELETON SOCKETS or as BONES on a skeletal
 *      mesh, and re-parents the mesh parts so that the export survives the import
 *      (`ArtSource/TOW/TOW_ART_CONTRACT.md`).
 *   2. The mesh is imported into Content at the path that `AFPMTowItem::mDeviceMeshAsset` points at.
 *   3. `FPM.Tow.Report` states that all three mount points resolved, AND states that its
 *      known-positive classifier check ran, rather than reporting it as not exercised.
 *
 * ⚠ `OriginStatus()` carries the same imperfect-fit note that `FFPMWristSlotHook` already states.
 * `Guard` is the least-wrong available value for a hook that PROVISIONS a catalog entry rather than
 * repairing a defect. Flagged, not hidden.
 */
class FFPMTowItemHook final : public IFPMFix
{
public:
	static FFPMTowItemHook& Get();

	virtual const TCHAR* Name() const override { return TEXT("tow-item"); }

	/** Catalog metadata only. Arm() itself touches no renderer, audio or input surface. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }
	virtual FPMDiag::EChannel Channel() const override;
	virtual void Arm() override;
	virtual void Disarm() override;

	/** See the class comment: the content half does not exist yet. */
	virtual bool DefaultArmed() const override { return false; }

	/**
	 * `FPM.Tow.Report`, callable on demand whether or not this hook is armed.
	 *
	 * ★ THE DEAD-INSTRUMENT ANSWER, STATED. This report prints COVERAGE, not a pass. It runs two
	 * classifier checks that are true code proof today, and it says plainly when the second one could
	 * not run.
	 *
	 *   KNOWN NEGATIVE, runs every time, with or without an asset. A mount point name that can never
	 *   legitimately exist must classify as `NotFound`. A `ClassifyMountPoint` that was accidentally
	 *   hardcoded to return a positive, which is the exact bug this guards against, fails this check
	 *   on every single run.
	 *
	 *   KNOWN POSITIVE, runs only when a mesh is loaded. The mesh's OWN first socket must classify as
	 *   `Socket`, and its OWN first bone must classify as `Bone`. Both names are read out of the
	 *   loaded asset, so the check cannot be satisfied by a hardcoded string. With no mesh loaded, the
	 *   report says NOT EXERCISED and names the reason. That is the honest state today, and it is
	 *   printed as MISSING COVERAGE rather than as a pass.
	 *
	 * The report resolves the asset with `LoadObject` rather than reading a possibly-unloaded
	 * pointer, specifically so that a correctly shipped asset that nothing else has touched yet is
	 * not misreported as missing.
	 */
	static void ReportNow();
};
