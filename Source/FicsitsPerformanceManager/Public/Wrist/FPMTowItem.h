// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Wrist/FPMWristItemBase.h"
#include "Core/FPMFixContract.h"
#include "FPMTowItem.generated.h"

class UStaticMeshComponent;
class UStaticMesh;

/**
 * ★ TOW (working name, Ant's ruling in tow-name.md: "Transit Overhead Winch", status WORK IN PROGRESS)
 * — THE FIRST CONCRETE WRIST ITEM, AND IT SHIPS INERT.
 *
 * Follows `AFPMWristItemBase` exactly rather than inventing a second pattern: that base already
 * attaches THIS ACTOR to the CHARACTER's hand socket inside `WristEquip_Implementation`
 * (`FPMWristItemBase.cpp:68`, `AttachToComponent(Mesh, SnapToTargetIncludingScale, Socket)`) — this
 * class supplies nothing but the device's OWN root component and the DEVICE-LOCAL socket question,
 * which is a different socket question from the one the base already answers
 * (`mSocketLeft`/`mSocketRight` — where on the CHARACTER this item attaches).
 *
 * ═══ THE THREE DEVICE SOCKETS, CONFIRMED FROM THE FROZEN glTF's OWN BYTES ═══
 *
 * `ArtSource/TOW/TOW_placeholder_v1.glb` (847,948 bytes, frozen at commit 4547b0c, byte-identical since
 * — see the findings file) carries three empty (mesh-less) nodes read directly out of its JSON chunk:
 * `SOCKET_Mount` (no translation — sits at the mesh's own local origin, i.e. the device's own pivot IS
 * the mount point), `SOCKET_LineExit` (matches node 27 `J4_Muzzle`'s position — the drum muzzle) and
 * `SOCKET_HookStow`. Unreal's Interchange importer (both the glTF and FBX translators feed the same
 * generic scene-node graph) strips a literal `SOCKET_` prefix from a node's name and turns the
 * remainder into a `UStaticMeshSocket` — `Engine/Plugins/Interchange/.../FactoryNodes/
 * InterchangeMeshFactoryNode.cpp:178-181`, `.../Import/Private/Mesh/InterchangeMeshHelper.cpp:760-807`
 * — so once imported, `FindSocket(TEXT("Mount"))` etc. is the real, on-by-default mechanism, not a
 * guess. THE CURRENT glb WILL NOT SURVIVE THAT IMPORT AS-IS THOUGH: its ~111 separate mesh parts sit
 * flat under the scene root with no parent nesting, and Interchange's socket-to-mesh association
 * requires a socket node to be a DESCENDANT of a mesh-bearing node once the file has more than one
 * mesh (`InterchangePipelineMeshesUtilities.cpp:456-512`) — so today, all three sockets would import as
 * dropped, silently, zero warning. Fix is an art-side Blender re-parent, not a code or click fix — see
 * the findings file section 2d for the full receipt. This class's own runtime checks (the .cpp) report
 * exactly that state rather than assuming the fix already happened.
 *
 * ═══ SHIPS INERT — SAME SHAPE AS FFPMThirdPersonToggle, SAME REASON ═══
 *
 * `Content/` currently holds one root `.uasset` and a `Settings/` folder — no `Wrist/` subfolder at
 * all (directory listing, findings file). `mDeviceMeshAsset` is a `TSoftObjectPtr`, never resolved in
 * the constructor — `ConstructorHelpers::FObjectFinder` would eager-load at CLASS-DEFAULT-OBJECT
 * construction time, which happens automatically the first time reflection touches this UCLASS and is
 * NOT gated by `IFPMFix::DefaultArmed()` at all (that only gates whether `Arm()` runs). A soft pointer
 * assigned by plain `FSoftObjectPath` never triggers a load until something explicitly resolves it, so
 * this class's CDO construction has zero cost and zero log spam regardless of arm state — the same
 * defensive-load shape `FPMThirdPersonToggle.h` documents for its two input assets.
 *
 * `FFPMTowItemHook` (below) is the piece that actually registers this class into the wrist catalog,
 * and IT ships `DefaultArmed() == false` for the same reason `FFPMThirdPersonToggle` does: registering
 * an item whose mesh resolves to nothing would let a caller "equip" something that renders as nothing
 * and gives the rope lane no anchor — worse than not registering, not better.
 */
UCLASS( Blueprintable )
class FICSITSPERFORMANCEMANAGER_API AFPMTowItem : public AFPMWristItemBase
{
	GENERATED_BODY()

public:
	AFPMTowItem();

	/**
	 * The exact strings Interchange will produce once the art-side re-parent (class comment above)
	 * lands: the glTF node's own name, minus the `SOCKET_` prefix. Named here once so nothing in this
	 * file or its .cpp risks a second, drifting copy of the string literal.
	 */
	static const FName SocketMount;
	static const FName SocketLineExit;
	static const FName SocketHookStow;

	//~Begin AActor
	virtual void BeginPlay() override;
	//~End AActor

	/**
	 * ★ WHAT THE ROPE LANE NEEDS FROM THIS SOCKET, ANSWERED DIRECTLY (findings file section 4 / the
	 * task's boot-question unblock). The rope lane's own architecture doc (lane-e2-rope-architecture.md)
	 * feeds `UInstancedStaticMeshComponent::BatchUpdateInstancesTransforms` a CURRENT and a PREVIOUS
	 * world-space position for its near (player-held) segment every frame. This is that position,
	 * resolved LIVE and NEVER CACHED — the item rides the character's socket every frame
	 * (`FPMWristItemBase.h`'s own `NeedTransform_Implementation` comment: a stored transform would be
	 * stale the instant the arm moves), so the rope lane must call this once per tick it needs a fresh
	 * anchor, exactly the way it already has to re-read the character's own transform every tick.
	 *
	 * Returns false (and leaves `OutTransform` at identity) when the mesh or the socket did not
	 * resolve. THE ROPE LANE MUST TREAT A FALSE RETURN AS "NO ANCHOR AVAILABLE" — never substitute a
	 * guessed offset from the character's hand socket, which is a different, unrelated transform.
	 */
	UFUNCTION( BlueprintCallable, Category = "FPM|Wrist|TOW" )
	bool GetLineExitTransform( FTransform& OutTransform ) const;

	/** Same contract as `GetLineExitTransform`, for the device's own mount/origin socket. */
	UFUNCTION( BlueprintCallable, Category = "FPM|Wrist|TOW" )
	bool GetMountTransform( FTransform& OutTransform ) const;

	/** Same contract as `GetLineExitTransform`, for where the hook parks when stowed. */
	UFUNCTION( BlueprintCallable, Category = "FPM|Wrist|TOW" )
	bool GetHookStowTransform( FTransform& OutTransform ) const;

protected:
	/** The device's own render mesh AND this actor's root component — the base's attach call
	 *  (`FPMWristItemBase.cpp:69`) snaps THIS component to the character's hand socket. */
	UPROPERTY( VisibleAnywhere, BlueprintReadOnly, Category = "FPM|Wrist|TOW" )
	TObjectPtr<UStaticMeshComponent> mDeviceMesh;

	/**
	 * Soft reference only — see the class comment for why the constructor never resolves this.
	 * Defaults to where the pipeline doc (findings file section 2g) says the imported asset should
	 * land; nothing exists at that path today.
	 */
	UPROPERTY( EditDefaultsOnly, BlueprintReadOnly, Category = "FPM|Wrist|TOW" )
	TSoftObjectPtr<UStaticMesh> mDeviceMeshAsset;

private:
	bool ResolveSocket( FName SocketName, FTransform& OutTransform ) const;
};

/**
 * ★ THE CATALOG ENTRY, wrapped as an `IFPMFix` for the same reason `FFPMWristSlotHook` is — consistency
 * with every other native surface in this mod, `DisarmAll`/`RearmAll` participation, an
 * `FPM.Fix.TowItem` toggle, and a crash-stamp roster entry, for free.
 *
 * SHIPS INERT — `DefaultArmed() == false`, same shape and same reason as `FFPMThirdPersonToggle`
 * (`Fixes/ModFeatures/FPMThirdPersonToggle.h`): its content half, the imported placeholder mesh, does
 * not exist (`AFPMTowItem`'s own class comment). Registering the class into the wrist catalog with a
 * mesh that resolves to nothing would let a caller equip an item that renders as nothing and anchors a
 * rope nowhere — worse than not registering it, because `SlotOccupied` would then also refuse a REAL
 * future item test from taking the slot for no visible benefit.
 *
 * FLIP `DefaultArmed()` TO true ONLY AFTER BOTH of these are independently true, in order:
 *   1. ArtSource/TOW's glb is re-parented (sockets nested under a mesh node) and re-exported — the
 *      art-side fix findings file section 2d names.
 *   2. The re-exported mesh is imported into Content per findings file section 2f/2g, at the path
 *      `AFPMTowItem::mDeviceMeshAsset` already points at.
 * `FPM.Tow.Report` (below) states plainly, every time it is run, whether both are true yet — it is the
 * single instrument that answers "is it safe to flip this switch" without re-deriving anything.
 *
 * ⚠ `OriginStatus()` carries the same imperfect-fit note `FFPMWristSlotHook` already states: `Guard` is
 * the least-wrong available value for a hook that PROVISIONS a catalog entry rather than repairing a
 * defect — flagged, not hidden.
 */
class FFPMTowItemHook final : public IFPMFix
{
public:
	static FFPMTowItemHook& Get();

	virtual const TCHAR* Name() const override { return TEXT("tow-item"); }

	/** Catalog metadata only — no renderer, audio or input surface touched by Arm() itself. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }
	virtual FPMDiag::EChannel Channel() const override;
	virtual void Arm() override;
	virtual void Disarm() override;

	/** See the class comment: the content half does not exist yet. */
	virtual bool DefaultArmed() const override { return false; }

	/**
	 * `FPM.Tow.Report` — callable on demand whether or not this hook is armed.
	 *
	 * ★ THE DEAD-INSTRUMENT ANSWER, STATED: this prints a NEGATIVE-CONTROL socket-resolution check
	 * that is TRUE CODE PROOF today, independent of whether the art asset exists — it probes a socket
	 * name that can never legitimately exist (`SOCKET_DoesNotExistProbe`, stripped to
	 * `DoesNotExistProbe`) against whatever mesh is currently loaded (including a null one) and the
	 * check FAILS if that ever resolves as found. A `ResolveSocket` that was accidentally hardcoded to
	 * `return true` — the exact bug this guards against — would fail this every single run. ★ The
	 * mirror: a mesh that DOES ship correctly, with all three real sockets present, must NOT be
	 * wrongly rejected by this same code path — the report resolves the soft pointer with
	 * `LoadSynchronous()` rather than reading a possibly-still-unloaded `Get()`, specifically so a
	 * correctly shipped asset that nothing else has touched yet is not misreported as missing.
	 */
	static void ReportNow();
};
