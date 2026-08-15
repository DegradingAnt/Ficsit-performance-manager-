// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Wrist/FPMWristItemBase.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Wrist/FPMWristSlotComponent.h"

#include "FGCharacterPlayer.h"

AFPMWristItemBase::AFPMWristItemBase()
{
	// A worn item never ticks on its own — it rides the owner's socket transform every frame via
	// attachment, not via a Tick that would recompute the same thing. See NeedTransform's comment:
	// the transform is derived, never stored or independently simulated.
	PrimaryActorTick.bCanEverTick = false;

	// This actor replicates (it is a networked, server-authoritative item — design §11.2.2's
	// "server-authoritative; replicated UPROPERTYs"), same as the slot component it attaches to.
	bReplicates = true;
	SetReplicatingMovement( false ); // attachment carries position; no independent movement to replicate
}

void AFPMWristItemBase::PostLoadGame_Implementation( int32 SaveVersion, int32 GameVersion )
{
	// ★ THE PERSISTENCE HANDSHAKE'S ITEM-SIDE HALF (design §11.2.3, point 3). `mOwnerCharacter` is a
	// UPROPERTY(SaveGame) reference to another saved actor, restored by the time PostLoadGame runs.
	// Registering here rather than equipping directly is deliberate: the slot component on
	// mOwnerCharacter may not exist yet (its own add-hook runs from BeginPlay, and load order between
	// the two is not guaranteed) — the pending-worn map is what makes the order not matter.
	if ( FPMDiag::IsOn( FPMDiag::EChannel::WristSlot ) )
	{
		UE_LOG( LogFicsitsPerformanceManager, Display,
			TEXT( "[FPM] wrist-slot: %s loaded from save (id=%s), owner=%s - registering pending-worn." ),
			*GetName(), *mWristItemId.ToString(), *GetNameSafe( mOwnerCharacter ) );
	}

	if ( mOwnerCharacter != nullptr )
	{
		UFPMWristSlotComponent::RegisterPendingWorn( mOwnerCharacter, this );
	}
	else
	{
		// Not a fix bug -- a saved item with no owner is a data state nothing in this design produces
		// deliberately, but "believed unreachable" still gets a log line rather than silence per the
		// project's own evidence discipline.
		UE_LOG( LogFicsitsPerformanceManager, Warning,
			TEXT( "[FPM] wrist-slot: %s loaded with a NULL mOwnerCharacter. It cannot register into the "
			      "pending-worn map and will sit un-equipped until something else claims it." ),
			*GetName() );
	}
}

void AFPMWristItemBase::WristEquip_Implementation( AFGCharacterPlayer* NewOwner )
{
	// ★ BOOKKEEPING ONLY. The slot component (`Server_Equip`) has already decided this is allowed —
	// SlotOccupied is checked THERE, not here. An item's own Equip is never a second gate (interface
	// header's own contract note).
	mOwnerCharacter = NewOwner;
	SetOwner( NewOwner );

	if ( NewOwner != nullptr )
	{
		if ( USkeletalMeshComponent* Mesh = NewOwner->GetMesh() )
		{
			const UFPMWristSlotComponent* Slot = NewOwner->FindComponentByClass<UFPMWristSlotComponent>();
			const bool bRightHanded = Slot != nullptr && Slot->IsRightHanded();
			const FName Socket = GetWristAttachSocket_Implementation( bRightHanded );
			AttachToComponent( Mesh, FAttachmentTransformRules::SnapToTargetIncludingScale, Socket );
		}
	}

	if ( FPMDiag::IsOn( FPMDiag::EChannel::WristSlot ) )
	{
		UE_LOG( LogFicsitsPerformanceManager, Display,
			TEXT( "[FPM] wrist-slot: %s equipped on %s." ), *GetName(), *GetNameSafe( NewOwner ) );
	}
}

void AFPMWristItemBase::WristUnequip_Implementation()
{
	DetachFromActor( FDetachmentTransformRules::KeepWorldTransform );

	if ( FPMDiag::IsOn( FPMDiag::EChannel::WristSlot ) )
	{
		UE_LOG( LogFicsitsPerformanceManager, Display,
			TEXT( "[FPM] wrist-slot: %s unequipped from %s." ), *GetName(), *GetNameSafe( mOwnerCharacter ) );
	}
}

bool AFPMWristItemBase::WristDeploy_Implementation()
{
	// The base has no cooldown or precondition of its own to refuse on -- a concrete item (the
	// grapple, Slice 5, explicitly NOT built here) may override this to add one. Always succeeds.
	if ( FPMDiag::IsOn( FPMDiag::EChannel::WristSlot ) )
	{
		UE_LOG( LogFicsitsPerformanceManager, Display, TEXT( "[FPM] wrist-slot: %s deployed." ), *GetName() );
	}
	return true;
}

void AFPMWristItemBase::WristRelease_Implementation()
{
	if ( FPMDiag::IsOn( FPMDiag::EChannel::WristSlot ) )
	{
		UE_LOG( LogFicsitsPerformanceManager, Display, TEXT( "[FPM] wrist-slot: %s released." ), *GetName() );
	}
}

FName AFPMWristItemBase::GetWristAttachSocket_Implementation( bool bRightHanded ) const
{
	// Resolved AT ATTACH TIME from data on the registration, never a hardcoded name at a callsite --
	// Ruling 11. B11 has not measured the real vanilla socket names against the shipped skeleton, so
	// mSocketLeft/mSocketRight's DEFAULTS are placeholders (the header states this).
	return bRightHanded ? mSocketRight : mSocketLeft;
}
