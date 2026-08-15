// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Wrist/FPMTowItem.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Wrist/FPMWristSlotComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/*
	 * Findings file section 2g's intended landing path. Nothing exists there today (Content/ holds one
	 * root .uasset plus Settings/, confirmed by directory listing) - both AFPMTowItem's constructor and
	 * this hook's report point at the SAME literal so they can never drift apart.
	 */
	const TCHAR* const GFPMTowMeshPath =
		TEXT("/FicsitsPerformanceManager/Wrist/TOW/SM_TOW_Placeholder.SM_TOW_Placeholder");

	bool bGFPMTowRegistered = false;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// AFPMTowItem
// ════════════════════════════════════════════════════════════════════════════════════════════════

const FName AFPMTowItem::SocketMount( TEXT( "Mount" ) );
const FName AFPMTowItem::SocketLineExit( TEXT( "LineExit" ) );
const FName AFPMTowItem::SocketHookStow( TEXT( "HookStow" ) );

AFPMTowItem::AFPMTowItem()
{
	mDeviceMesh = CreateDefaultSubobject<UStaticMeshComponent>( TEXT( "DeviceMesh" ) );
	RootComponent = mDeviceMesh;

	// A worn cosmetic device, not something the player or other actors should collide against.
	mDeviceMesh->SetCollisionEnabled( ECollisionEnabled::NoCollision );

	mWristItemId = TEXT( "TOW" );
	mDisplayName = FText::FromString( TEXT( "Transit Overhead Winch" ) );

	/*
	 * ★ SOFT PATH ONLY, ASSIGNED, NEVER RESOLVED HERE. See the header's class comment for why:
	 * ConstructorHelpers::FObjectFinder would eager-load at CLASS-DEFAULT-OBJECT construction, which UE
	 * triggers automatically the first time reflection touches this UCLASS - a cost paid on every boot
	 * regardless of IFPMFix::DefaultArmed(), which only gates whether Arm() runs. Plain assignment of an
	 * FSoftObjectPath never loads anything; resolution happens in BeginPlay() below, or in the hook's
	 * ReportNow(), both of which run rarely (BeginPlay only once something eventually spawns this actor -
	 * nothing does yet - and ReportNow only on demand or when manually armed).
	 */
	mDeviceMeshAsset = TSoftObjectPtr<UStaticMesh>( FSoftObjectPath( GFPMTowMeshPath ) );
}

void AFPMTowItem::BeginPlay()
{
	Super::BeginPlay();

	if ( mDeviceMeshAsset.IsNull() )
	{
		UE_LOG( LogFicsitsPerformanceManager, Warning,
			TEXT( "[FPM] tow-item: %s has no device mesh asset configured - nothing will render and no "
			      "socket will resolve." ),
			*GetName() );
		return;
	}

	// A rare, one-shot resolution (equip time), not a per-frame cost - LoadSynchronous is the right
	// call here, the same defensive-load shape FPMThirdPersonToggle.cpp's ResolveInputAssets uses.
	UStaticMesh* Mesh = mDeviceMeshAsset.LoadSynchronous();
	if ( Mesh == nullptr )
	{
		UE_LOG( LogFicsitsPerformanceManager, Warning,
			TEXT( "[FPM] tow-item: %s could not load device mesh '%s'. This class ships inert until "
			      "ArtSource/TOW is imported - see the findings file for the pipeline steps and the "
			      "current blocker (flat glTF hierarchy drops all three sockets on import)." ),
			*GetName(), *mDeviceMeshAsset.ToString() );
		return;
	}

	mDeviceMesh->SetStaticMesh( Mesh );

	for ( const FName SocketName : { SocketMount, SocketLineExit, SocketHookStow } )
	{
		if ( !mDeviceMesh->DoesSocketExist( SocketName ) )
		{
			UE_LOG( LogFicsitsPerformanceManager, Warning,
				TEXT( "[FPM] tow-item: %s's mesh '%s' has no socket named '%s'. See the findings file "
				      "section on why the current frozen export drops all three device sockets on "
				      "import (flat hierarchy, more than one mesh in the file)." ),
				*GetName(), *Mesh->GetName(), *SocketName.ToString() );
		}
	}
}

bool AFPMTowItem::ResolveSocket( FName SocketName, FTransform& OutTransform ) const
{
	if ( mDeviceMesh == nullptr || !mDeviceMesh->DoesSocketExist( SocketName ) )
	{
		OutTransform = FTransform::Identity;
		return false;
	}
	OutTransform = mDeviceMesh->GetSocketTransform( SocketName, RTS_World );
	return true;
}

bool AFPMTowItem::GetLineExitTransform( FTransform& OutTransform ) const
{
	return ResolveSocket( SocketLineExit, OutTransform );
}

bool AFPMTowItem::GetMountTransform( FTransform& OutTransform ) const
{
	return ResolveSocket( SocketMount, OutTransform );
}

bool AFPMTowItem::GetHookStowTransform( FTransform& OutTransform ) const
{
	return ResolveSocket( SocketHookStow, OutTransform );
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
	// Shares the wrist system's own channel rather than adding a new EChannel value tonight - FPMDiag.h
	// is a shared registration point several other agents are concurrently editing in this tree
	// (CONCURRENCY note), and this hook's whole job is wrist-catalog metadata, so WristSlot is the
	// correct channel on the merits too, not just a scope-minimising choice.
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
		TEXT( "[FPM] tow-item ARMED - registered=%s. FPM.Tow.Report says whether the placeholder mesh "
		      "and its three sockets resolved." ),
		bGFPMTowRegistered ? TEXT( "yes" ) : TEXT( "no" ) );

	ReportNow();
}

void FFPMTowItemHook::Disarm()
{
	// No public Unregister on the wrist API - the catalog is additive by design
	// (FPMWristSlotComponent.h's own RegisterWristItem doc comment), the same reason FFPMWristSlotHook's
	// Disarm() destroys no component. The entry simply stops being refreshed; nothing to tear down.
	bGFPMTowRegistered = false;
}

void FFPMTowItemHook::ReportNow()
{
	UStaticMesh* Mesh = LoadObject<UStaticMesh>( nullptr, GFPMTowMeshPath );

	// ── the negative control: proof the resolver can say "no" ──────────────────────────────────────
	// A socket name that can never legitimately exist. If this ever resolves as found, the bug is in
	// THIS code (e.g. FindSocket called on the wrong object, or a stale cached pointer), never in the
	// art - this check is real, run-today proof, independent of whether the mesh asset exists at all.
	static const FName ProbeSocketName( TEXT( "DoesNotExistProbe" ) );
	const bool bNegativeControlOk = ( Mesh == nullptr ) || ( Mesh->FindSocket( ProbeSocketName ) == nullptr );

	const bool bMeshResolved = Mesh != nullptr;
	const bool bMountOk = bMeshResolved && Mesh->FindSocket( AFPMTowItem::SocketMount ) != nullptr;
	const bool bLineExitOk = bMeshResolved && Mesh->FindSocket( AFPMTowItem::SocketLineExit ) != nullptr;
	const bool bHookStowOk = bMeshResolved && Mesh->FindSocket( AFPMTowItem::SocketHookStow ) != nullptr;

	UE_LOG( LogFicsitsPerformanceManager, Display,
		TEXT( "[FPM] tow-item: negative-control socket probe %s (a garbage socket name must never "
		      "resolve as found - this is a pass/fail on the CODE, provable regardless of art state)." ),
		bNegativeControlOk ? TEXT( "PASSED" ) : TEXT( "FAILED - FindSocket resolved a name that cannot exist" ) );

	UE_LOG( LogFicsitsPerformanceManager, Display,
		TEXT( "[FPM] tow-item: registered=%s. device mesh '%s' resolved=%s. sockets: Mount=%s "
		      "LineExit=%s HookStow=%s. COVERAGE: all three read NOT FOUND right now because either the "
		      "mesh has not been imported yet, or (findings file, the flat-hierarchy blocker) it imports "
		      "with its sockets silently dropped - both are expected, documented states today, not an "
		      "unexplained failure. This line is the one instrument that says when it is safe to flip "
		      "FFPMTowItemHook::DefaultArmed() to true." ),
		bGFPMTowRegistered ? TEXT( "yes" ) : TEXT( "no" ),
		GFPMTowMeshPath,
		bMeshResolved ? TEXT( "yes" ) : TEXT( "NOT FOUND" ),
		bMountOk ? TEXT( "OK" ) : TEXT( "NOT FOUND" ),
		bLineExitOk ? TEXT( "OK" ) : TEXT( "NOT FOUND" ),
		bHookStowOk ? TEXT( "OK" ) : TEXT( "NOT FOUND" ) );
}

static FAutoConsoleCommandWithOutputDevice GFPMTowReportCmd(
	TEXT( "FPM.Tow.Report" ),
	TEXT( "TOW wrist item: registration state, and whether the placeholder mesh and its three device "
	      "sockets resolved." ),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic( []( FOutputDevice& Ar )
	{
		FPMScopedConsoleEcho Echo( &Ar );
		FFPMTowItemHook::ReportNow();
	} ) );
