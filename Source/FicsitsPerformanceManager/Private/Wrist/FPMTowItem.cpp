// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Wrist/FPMTowItem.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Wrist/FPMWristSlotComponent.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/*
	 * The intended landing paths. NOTHING EXISTS AT EITHER OF THEM TODAY. Both the actor and the hook
	 * report point at these same literals, so the two can never drift apart.
	 */
	const TCHAR* const GFPMTowMeshPath =
		TEXT("/FicsitsPerformanceManager/Wrist/TOW/SK_TOW_Placeholder.SK_TOW_Placeholder");

	const TCHAR* const GFPMTowDeployAnimPath =
		TEXT("/FicsitsPerformanceManager/Wrist/TOW/AS_TOW_HookDeploy.AS_TOW_HookDeploy");

	bool bGFPMTowRegistered = false;

	/*
	 * Printed by the coverage lines. Three words, never two, because the whole point of
	 * EFPMTowMountResolution is that "found" hides which of the two ways it was found.
	 */
	const TCHAR* MountResolutionText( EFPMTowMountResolution Resolution )
	{
		switch ( Resolution )
		{
		case EFPMTowMountResolution::Socket:  return TEXT( "SOCKET" );
		case EFPMTowMountResolution::Bone:    return TEXT( "BONE (no socket authored)" );
		default:                              return TEXT( "NOT FOUND" );
		}
	}

	const TCHAR* HookStateText( EFPMTowHookState State )
	{
		switch ( State )
		{
		case EFPMTowHookState::Deploying: return TEXT( "Deploying" );
		case EFPMTowHookState::Deployed:  return TEXT( "Deployed" );
		case EFPMTowHookState::Stowing:   return TEXT( "Stowing" );
		default:                          return TEXT( "Stowed" );
		}
	}
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// AFPMTowItem
// ════════════════════════════════════════════════════════════════════════════════════════════════

const FName AFPMTowItem::MountName( TEXT( "Mount" ) );
const FName AFPMTowItem::LineExitName( TEXT( "LineExit" ) );
const FName AFPMTowItem::HookStowName( TEXT( "HookStow" ) );

AFPMTowItem::AFPMTowItem()
{
	mDeviceMesh = CreateDefaultSubobject<USkeletalMeshComponent>( TEXT( "DeviceMesh" ) );
	RootComponent = mDeviceMesh;

	// A worn cosmetic device. The player and other actors must not collide against it.
	mDeviceMesh->SetCollisionEnabled( ECollisionEnabled::NoCollision );

	mWristItemId = TEXT( "TOW" );
	mDisplayName = FText::FromString( TEXT( "Transit Overhead Winch" ) );

	/*
	 * ★ SOFT PATHS ONLY, ASSIGNED, NEVER RESOLVED HERE. See the header's class comment for why.
	 * ConstructorHelpers::FObjectFinder would eager-load at CLASS-DEFAULT-OBJECT construction, which
	 * UE triggers automatically the first time reflection touches this UCLASS. That cost is paid on
	 * every boot regardless of IFPMFix::DefaultArmed(), which only gates whether Arm() runs. Plain
	 * assignment of an FSoftObjectPath loads nothing. Resolution happens in ResolveAssets(), called
	 * from BeginPlay(), or in the hook's ReportNow(). Both run rarely.
	 *
	 * ⚠ The engine also forbids the single-node animation calls here by its own doc comment
	 * (SkeletalMeshComponent.h:1160-1231, "not safe to be used during construction script"). This
	 * constructor makes none of them.
	 */
	mDeviceMeshAsset = TSoftObjectPtr<USkeletalMesh>( FSoftObjectPath( GFPMTowMeshPath ) );
	mDeployAnimAsset = TSoftObjectPtr<UAnimSequence>( FSoftObjectPath( GFPMTowDeployAnimPath ) );
}

void AFPMTowItem::GetLifetimeReplicatedProps( TArray<FLifetimeProperty>& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );

	DOREPLIFETIME( AFPMTowItem, mHookState );
}

void AFPMTowItem::BeginPlay()
{
	Super::BeginPlay();

	ResolveAssets();
}

void AFPMTowItem::ResolveAssets()
{
	if ( mDeviceMeshAsset.IsNull() )
	{
		UE_LOG( LogFicsitsPerformanceManager, Warning,
			TEXT( "[FPM] tow-item: %s has no device mesh asset configured. Nothing will render and no "
			      "mount point will resolve." ),
			*GetName() );
		return;
	}

	// A rare, one-shot resolution at equip time, not a per-frame cost. LoadSynchronous is the right
	// call here, the same defensive-load shape FPMThirdPersonToggle.cpp's ResolveInputAssets uses.
	USkeletalMesh* Mesh = mDeviceMeshAsset.LoadSynchronous();
	if ( Mesh == nullptr )
	{
		UE_LOG( LogFicsitsPerformanceManager, Warning,
			TEXT( "[FPM] tow-item: %s could not load device mesh '%s'. This class ships inert until "
			      "ArtSource/TOW is authored as a SKELETAL mesh and imported. See "
			      "ArtSource/TOW/TOW_ART_CONTRACT.md." ),
			*GetName(), *mDeviceMeshAsset.ToString() );
		return;
	}

	mDeviceMesh->SetSkeletalMeshAsset( Mesh );

	/*
	 * ★ PER MOUNT POINT COVERAGE, NOT A PASS. Each of the three is reported as SOCKET, BONE or NOT
	 * FOUND. BONE is deliberately not collapsed into a positive: it means the art shipped the mount
	 * point as a joint and never authored a socket, which still works but is not what the contract
	 * asked for.
	 */
	for ( const FName MountPointName : { MountName, LineExitName, HookStowName } )
	{
		const EFPMTowMountResolution Resolution = ClassifyMountPoint( Mesh, MountPointName );

		if ( Resolution == EFPMTowMountResolution::NotFound )
		{
			UE_LOG( LogFicsitsPerformanceManager, Warning,
				TEXT( "[FPM] tow-item: %s's mesh '%s' has no socket AND no bone named '%s'. Nothing can "
				      "attach there. See ArtSource/TOW/TOW_ART_CONTRACT.md." ),
				*GetName(), *Mesh->GetName(), *MountPointName.ToString() );
		}
		else
		{
			UE_LOG( LogFicsitsPerformanceManager, Display,
				TEXT( "[FPM] tow-item: %s mount point '%s' resolved as %s." ),
				*GetName(), *MountPointName.ToString(), MountResolutionText( Resolution ) );
		}
	}

	// ── the two poses ─────────────────────────────────────────────────────────────────────────────
	if ( mDeployAnimAsset.IsNull() )
	{
		UE_LOG( LogFicsitsPerformanceManager, Warning,
			TEXT( "[FPM] tow-item: %s has no deploy AnimSequence configured. The hook will change STATE "
			      "but it will not move: deploy and stow land on their end state instantly." ),
			*GetName() );
		return;
	}

	UAnimSequence* Anim = mDeployAnimAsset.LoadSynchronous();
	if ( Anim == nullptr )
	{
		UE_LOG( LogFicsitsPerformanceManager, Warning,
			TEXT( "[FPM] tow-item: %s could not load deploy AnimSequence '%s'. The hook will change "
			      "STATE but it will not move." ),
			*GetName(), *mDeployAnimAsset.ToString() );
		return;
	}

	/*
	 * ★ THE CODE ROUTE, NOT AN AnimBlueprint ASSET ROUTE. Ant is not fluent in the editor yet, so an
	 * asset route costs far more of her time than this does. AnimationSingleNode is the mode that
	 * makes SetAnimation / SetPosition / SetPlayRate / Play the driver
	 * (SkeletalMeshComponent.h:1155, EAnimationMode at :183-191).
	 */
	mDeviceMesh->SetAnimationMode( EAnimationMode::AnimationSingleNode );
	mDeviceMesh->SetAnimation( Anim );

	mPoseLengthSeconds = Anim->GetPlayLength();
	bAnimReady = mPoseLengthSeconds > 0.0f;

	if ( !bAnimReady )
	{
		UE_LOG( LogFicsitsPerformanceManager, Warning,
			TEXT( "[FPM] tow-item: %s's deploy AnimSequence '%s' reports a play length of %.3f seconds. "
			      "A zero-length pose cannot snap, so the hook will land on its end state instantly." ),
			*GetName(), *Anim->GetName(), mPoseLengthSeconds );
		return;
	}

	// Park on the closed pose. The item is worn stowed.
	mDeviceMesh->SetPosition( 0.0f );
	mDeviceMesh->Stop();

	UE_LOG( LogFicsitsPerformanceManager, Display,
		TEXT( "[FPM] tow-item: %s snap pose ready. '%s', %.3f s at rate 1.0. Forward snaps the hook "
		      "open, a NEGATIVE play rate snaps it closed." ),
		*GetName(), *Anim->GetName(), mPoseLengthSeconds );
}

// ── the classifier, and the mount point getters that use it ──────────────────────────────────────

EFPMTowMountResolution AFPMTowItem::ClassifyMountPoint( const USkeletalMesh* Mesh, FName MountPointName )
{
	if ( Mesh == nullptr || MountPointName.IsNone() )
	{
		return EFPMTowMountResolution::NotFound;
	}

	// Searches the mesh's own socket list AND the skeleton's (SkeletalMesh.cpp:4759-4799).
	if ( Mesh->FindSocket( MountPointName ) != nullptr )
	{
		return EFPMTowMountResolution::Socket;
	}

	// ⚠ THE TRAP. A bone reaching this line is exactly what DoesSocketExist would have called a
	// socket (SkinnedMeshComponent.cpp:3410-3435). It is a different art state and it gets a
	// different answer.
	if ( Mesh->GetRefSkeleton().FindBoneIndex( MountPointName ) != INDEX_NONE )
	{
		return EFPMTowMountResolution::Bone;
	}

	return EFPMTowMountResolution::NotFound;
}

bool AFPMTowItem::ResolveMountPoint( FName MountPointName, FTransform& OutTransform ) const
{
	const USkeletalMesh* Mesh = ( mDeviceMesh != nullptr ) ? mDeviceMesh->GetSkeletalMeshAsset() : nullptr;

	if ( ClassifyMountPoint( Mesh, MountPointName ) == EFPMTowMountResolution::NotFound )
	{
		OutTransform = FTransform::Identity;
		return false;
	}

	// GetSocketTransform resolves a socket OR a bone (SkinnedMeshComponent.cpp:3221-3262), which is
	// why both positive classifications are served by this one call.
	OutTransform = mDeviceMesh->GetSocketTransform( MountPointName, RTS_World );
	return true;
}

bool AFPMTowItem::GetLineExitTransform( FTransform& OutTransform ) const
{
	return ResolveMountPoint( LineExitName, OutTransform );
}

bool AFPMTowItem::GetMountTransform( FTransform& OutTransform ) const
{
	return ResolveMountPoint( MountName, OutTransform );
}

bool AFPMTowItem::GetHookStowTransform( FTransform& OutTransform ) const
{
	return ResolveMountPoint( HookStowName, OutTransform );
}

// ── the two poses ────────────────────────────────────────────────────────────────────────────────

float AFPMTowItem::StartPose( bool bOpening )
{
	if ( !bAnimReady || mDeviceMesh == nullptr )
	{
		return 0.0f;
	}

	/*
	 * A rate at or below zero would leave the hook stuck mid-snap forever, and it would hand
	 * BeginTransition an infinite or negative delay to time against. Refuse the value here rather
	 * than propagate it.
	 */
	const float Rate = ( mPoseRate > 0.0f ) ? mPoseRate : 1.0f;

	// Reverse IS a negative play rate: FAnimSingleNodeInstanceProxy::SetReverse does exactly
	// PlayRate = -FMath::Abs(PlayRate) (AnimSingleNodeInstanceProxy.cpp:246-256). Playing backward
	// from position 0 would end before it began, so the start position moves with the direction.
	mDeviceMesh->SetPosition( bOpening ? 0.0f : mPoseLengthSeconds );
	mDeviceMesh->SetPlayRate( bOpening ? Rate : -Rate );
	mDeviceMesh->Play( false );

	// GetPlayLength is the length AT SPEED 1.0 by its own doc comment (AnimSequenceBase.h:84-86).
	return mPoseLengthSeconds / Rate;
}

void AFPMTowItem::ApplyEndPose( bool bOpen )
{
	if ( !bAnimReady || mDeviceMesh == nullptr )
	{
		return;
	}

	mDeviceMesh->Stop();
	mDeviceMesh->SetPosition( bOpen ? mPoseLengthSeconds : 0.0f );
}

void AFPMTowItem::FinishPose()
{
	if ( mHookState == EFPMTowHookState::Deploying )
	{
		mHookState = EFPMTowHookState::Deployed;
		ApplyEndPose( true );
	}
	else if ( mHookState == EFPMTowHookState::Stowing )
	{
		mHookState = EFPMTowHookState::Stowed;
		ApplyEndPose( false );
	}

	if ( FPMDiag::IsOn( FPMDiag::EChannel::WristSlot ) )
	{
		UE_LOG( LogFicsitsPerformanceManager, Display,
			TEXT( "[FPM] tow-item: %s hook is now %s." ), *GetName(), HookStateText( mHookState ) );
	}
}

void AFPMTowItem::OnRep_HookState()
{
	/*
	 * ★ THE CLIENT HALF, AND IT RUNS NO TIMER OF ITS OWN. UFPMWristSlotComponent calls WristDeploy
	 * and WristRelease on the SERVER only (FPMWristSlotComponent.cpp:791, :824), so the transition
	 * state and the end state both arrive here as replication. A client-side timer would write a
	 * replicated property locally and then fight the next update.
	 *
	 * A LATE JOINER receives only the end state, never the transition. That is why Deployed and
	 * Stowed park the pose instead of playing it: a hook that snapped open in front of a player who
	 * joined an hour later would be a lie about what just happened.
	 */
	switch ( mHookState )
	{
	case EFPMTowHookState::Deploying: StartPose( true );   break;
	case EFPMTowHookState::Stowing:   StartPose( false );  break;
	case EFPMTowHookState::Deployed:  ApplyEndPose( true );  break;
	case EFPMTowHookState::Stowed:    ApplyEndPose( false ); break;
	}
}

void AFPMTowItem::BeginTransition( bool bOpening )
{
	mHookState = bOpening ? EFPMTowHookState::Deploying : EFPMTowHookState::Stowing;

	const float Seconds = StartPose( bOpening );
	UWorld* World = GetWorld();

	if ( Seconds > 0.0f && World != nullptr )
	{
		// Reusing the one handle cancels any transition still pending. FTimerManager::InternalSetTimer
		// clears a handle it already knows before re-adding it (TimerManager.cpp:588-593), so a deploy
		// followed straight away by a release cannot leave two timers racing.
		World->GetTimerManager().SetTimer( mPoseTimer, this, &AFPMTowItem::FinishPose, Seconds, false );
	}
	else
	{
		// No pose asset, or no world to time against. Land on the end state inside this same call, so
		// that no caller ever observes a hook parked in a transition state forever.
		FinishPose();
	}
}

bool AFPMTowItem::WristDeploy_Implementation()
{
	if ( !Super::WristDeploy_Implementation() )
	{
		return false;
	}

	// Idempotent. A second deploy on an already-open hook is not a refusal, it is a no-op.
	if ( mHookState == EFPMTowHookState::Deployed || mHookState == EFPMTowHookState::Deploying )
	{
		return true;
	}

	BeginTransition( true );
	return true;
}

void AFPMTowItem::WristRelease_Implementation()
{
	Super::WristRelease_Implementation();

	if ( mHookState == EFPMTowHookState::Stowed || mHookState == EFPMTowHookState::Stowing )
	{
		return;
	}

	BeginTransition( false );
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// FFPMTowItemHook
// ════════════════════════════════════════════════════════════════════════════════════════════════

FFPMTowItemHook& FFPMTowItemHook::Get()
{
	static FFPMTowItemHook Instance;
	return Instance;
}

FPMDiag::EChannel FFPMTowItemHook::Channel() const
{
	// Shares the wrist system's own channel rather than adding a new EChannel value. FPMDiag.h is a
	// shared registration point that several other agents edit concurrently in this tree, and this
	// hook's whole job is wrist-catalog metadata, so WristSlot is the correct channel on the merits
	// too, not only as a scope-minimising choice.
	return FPMDiag::EChannel::WristSlot;
}

void FFPMTowItemHook::Arm()
{
	EFPMWristRefusal Refusal = EFPMWristRefusal::None;
	bGFPMTowRegistered = UFPMWristSlotComponent::RegisterWristItem(
		TEXT( "FicsitsPerformanceManager" ), TEXT( "TOW" ),
		TSoftClassPtr<AActor>( AFPMTowItem::StaticClass() ),
		FText::FromString( TEXT( "Transit Overhead Winch" ) ),
		FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR, Refusal );

	UE_LOG( LogFicsitsPerformanceManager, Display,
		TEXT( "[FPM] tow-item ARMED. registered=%s. FPM.Tow.Report states whether the placeholder mesh "
		      "and its three mount points resolved." ),
		bGFPMTowRegistered ? TEXT( "yes" ) : TEXT( "no" ) );

	ReportNow();
}

void FFPMTowItemHook::Disarm()
{
	// The wrist API has no public Unregister. The catalog is additive by design
	// (FPMWristSlotComponent.h's own RegisterWristItem doc comment), the same reason
	// FFPMWristSlotHook's Disarm() destroys no component. The entry stops being refreshed. There is
	// nothing to tear down.
	bGFPMTowRegistered = false;
}

void FFPMTowItemHook::ReportNow()
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>( nullptr, GFPMTowMeshPath );

	/*
	 * ══ CHECK 1, THE KNOWN NEGATIVE. Runs every time, asset or no asset. ═══════════════════════════
	 *
	 * A mount point name that can never legitimately exist must classify as NotFound. If this ever
	 * classifies as anything else, the bug is in THIS code, never in the art: a ClassifyMountPoint
	 * hardcoded to a positive, or a FindSocket called against the wrong object. This is real,
	 * run-today proof, independent of whether any asset exists.
	 *
	 * The probe name is deliberately unlike the three real mount point names, so that a grep counting
	 * real mount point rows in FactoryGame.log can never pick this line up as a fourth one.
	 */
	static const FName ProbeMountPointName( TEXT( "FpmClassifierNegativeProbe" ) );
	const bool bNegativeControlOk =
		AFPMTowItem::ClassifyMountPoint( Mesh, ProbeMountPointName ) == EFPMTowMountResolution::NotFound;

	/*
	 * ══ CHECK 2, THE KNOWN POSITIVE. Runs only when a mesh is loaded. ══════════════════════════════
	 *
	 * The mirror of check 1: a classifier that always says NotFound would pass check 1 forever and
	 * would silently reject a mesh that shipped correctly. Both probe names are READ OUT OF THE
	 * LOADED ASSET, so no hardcoded string can satisfy this.
	 *
	 * With no mesh loaded this cannot run at all, and the report says so. That is missing coverage,
	 * stated as missing coverage. It is not a pass.
	 */
	bool bSocketPositiveRan = false;
	bool bSocketPositiveOk = false;
	bool bBonePositiveRan = false;
	bool bBonePositiveOk = false;

	if ( Mesh != nullptr )
	{
		if ( Mesh->NumSockets() > 0 )
		{
			if ( const USkeletalMeshSocket* FirstSocket = Mesh->GetSocketByIndex( 0 ) )
			{
				bSocketPositiveRan = true;
				bSocketPositiveOk = AFPMTowItem::ClassifyMountPoint( Mesh, FirstSocket->SocketName )
					== EFPMTowMountResolution::Socket;
			}
		}

		if ( Mesh->GetRefSkeleton().GetNum() > 0 )
		{
			const FName FirstBoneName = Mesh->GetRefSkeleton().GetBoneName( 0 );
			bBonePositiveRan = true;
			// A root bone that ALSO carries a socket of the same name would classify as Socket, which
			// is correct behaviour and not a failure of this check. Assert the classifier found it,
			// not which of the two it chose.
			bBonePositiveOk = AFPMTowItem::ClassifyMountPoint( Mesh, FirstBoneName )
				!= EFPMTowMountResolution::NotFound;
		}
	}

	const bool bMeshResolved = Mesh != nullptr;
	const EFPMTowMountResolution MountRes = AFPMTowItem::ClassifyMountPoint( Mesh, AFPMTowItem::MountName );
	const EFPMTowMountResolution LineExitRes = AFPMTowItem::ClassifyMountPoint( Mesh, AFPMTowItem::LineExitName );
	const EFPMTowMountResolution HookStowRes = AFPMTowItem::ClassifyMountPoint( Mesh, AFPMTowItem::HookStowName );

	int32 Resolved = 0;
	for ( const EFPMTowMountResolution Res : { MountRes, LineExitRes, HookStowRes } )
	{
		if ( Res != EFPMTowMountResolution::NotFound )
		{
			++Resolved;
		}
	}

	UE_LOG( LogFicsitsPerformanceManager, Display,
		TEXT( "[FPM] tow-item CLASSIFIER: known-negative %s. known-positive socket %s. known-positive "
		      "bone %s. A known-positive that reads NOT EXERCISED means no mesh is loaded, so half of "
		      "this instrument is unproven today." ),
		bNegativeControlOk ? TEXT( "PASSED" ) : TEXT( "FAILED, a name that cannot exist classified as found" ),
		bSocketPositiveRan ? ( bSocketPositiveOk ? TEXT( "PASSED" ) : TEXT( "FAILED, a real socket did not classify as SOCKET" ) )
			: TEXT( "NOT EXERCISED" ),
		bBonePositiveRan ? ( bBonePositiveOk ? TEXT( "PASSED" ) : TEXT( "FAILED, a real bone classified as NOT FOUND" ) )
			: TEXT( "NOT EXERCISED" ) );

	UE_LOG( LogFicsitsPerformanceManager, Display,
		TEXT( "[FPM] tow-item COVERAGE: registered=%s. device mesh '%s' resolved=%s. mount points "
		      "%d of 3 resolved: %s=%s %s=%s %s=%s. A BONE result means the art shipped a joint and "
		      "authored no socket, which works but is not what ArtSource/TOW/TOW_ART_CONTRACT.md asks "
		      "for. All three read NOT FOUND today because no skeletal mesh has been imported. That is "
		      "a documented state, not an unexplained failure. This line is the one instrument that "
		      "says when it is safe to flip FFPMTowItemHook::DefaultArmed() to true." ),
		bGFPMTowRegistered ? TEXT( "yes" ) : TEXT( "no" ),
		GFPMTowMeshPath,
		bMeshResolved ? TEXT( "yes" ) : TEXT( "NOT FOUND" ),
		Resolved,
		*AFPMTowItem::MountName.ToString(), MountResolutionText( MountRes ),
		*AFPMTowItem::LineExitName.ToString(), MountResolutionText( LineExitRes ),
		*AFPMTowItem::HookStowName.ToString(), MountResolutionText( HookStowRes ) );

	UAnimSequence* Anim = LoadObject<UAnimSequence>( nullptr, GFPMTowDeployAnimPath );
	UE_LOG( LogFicsitsPerformanceManager, Display,
		TEXT( "[FPM] tow-item POSE: deploy AnimSequence '%s' resolved=%s, length=%.3f s at rate 1.0. "
		      "Without it the hook still changes state, it just does not move." ),
		GFPMTowDeployAnimPath,
		Anim != nullptr ? TEXT( "yes" ) : TEXT( "NOT FOUND" ),
		Anim != nullptr ? Anim->GetPlayLength() : 0.0f );
}

static FAutoConsoleCommandWithOutputDevice GFPMTowReportCmd(
	TEXT( "FPM.Tow.Report" ),
	TEXT( "TOW wrist item: registration state, the classifier's own known-positive and known-negative "
	      "checks, and whether the placeholder skeletal mesh, its three device mount points and its "
	      "deploy pose resolved." ),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic( []( FOutputDevice& Ar )
	{
		FPMScopedConsoleEcho Echo( &Ar );
		FFPMTowItemHook::ReportNow();
	} ) );
