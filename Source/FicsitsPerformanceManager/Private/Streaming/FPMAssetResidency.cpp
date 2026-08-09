// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Streaming/FPMAssetResidency.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"

#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	/*
	 * The four platform-service icons vanilla's BPW_UserIcon can ask for on PC, copied VERBATIM from the
	 * shipped asset rather than reconstructed: `BPW_UserIcon.json:18-47` in the FModel export, keys
	 * Steam / Epic / Xbox / PSN. Re-read from those bytes on 2026-08-09 before this file was written.
	 *
	 * All four are reachable on PC — crossplay means a Steam client can be shown an Epic, Xbox or PSN peer.
	 * The PS5/XSX-only branches of `GetPlatformIcon` return different textures and are unreachable on this
	 * platform, so they are deliberately not pinned.
	 *
	 * ⚠ The name is prefixed because this module is a UNITY BUILD: file-local names are not file-local here,
	 * and an anonymous namespace does not save you. FPMFixContract.h:166-171 records the C2374 this avoids.
	 */
	const TCHAR* const GFPMResidencyIconPaths[] =
	{
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_Steam_128.TXUI_Steam_128"),
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_Epic_128.TXUI_Epic_128"),
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_XBOX_128.TXUI_XBOX_128"),
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_PlayStation_128.TXUI_PlayStation_128"),
	};

	constexpr int32 GFPMResidencyNumIcons = static_cast<int32>(UE_ARRAY_COUNT(GFPMResidencyIconPaths));
}

FFPMAssetResidency& FFPMAssetResidency::Get()
{
	static FFPMAssetResidency Instance;
	return Instance;
}

void FFPMAssetResidency::Arm()
{
	// No hook to install. The work is the pin. Try now, and if the asset manager is not up yet, retry every
	// frame until it is -- see RetryTick's comment for the measurement that made this necessary.
	EnsurePinned(TEXT("startup"));

	if (!PinHandle.IsValid() && !RetryHandle.IsValid())
	{
		RetryHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FFPMAssetResidency::RetryTick), 0.f);
	}
}

bool FFPMAssetResidency::RetryTick(float)
{
	if (PinHandle.IsValid())
	{
		RetryHandle.Reset();
		return false;          // unregister: the work is done
	}

	EnsurePinned(TEXT("early retry"));

	if (PinHandle.IsValid())
	{
		RetryHandle.Reset();
		return false;
	}
	return true;               // keep trying
}

void FFPMAssetResidency::OnWorldLoad(UWorld* World)
{
	EnsurePinned(TEXT("world load"));
}

void FFPMAssetResidency::EnsurePinned(const TCHAR* Moment)
{
	if (PinHandle.IsValid())
	{
		return;   // already pinned, or already in flight
	}

	if (!UAssetManager::IsInitialized())
	{
		// NEVER FAIL SILENTLY -- but ONCE, not once per frame. This is now called from a per-frame retry
		// ticker, and the first version's unconditional Display line would have written one entry per frame
		// for the whole of engine init. A diagnostic that floods the log it writes to destroys the log's
		// usefulness, which is a heavier cost than the one it was guarding against.
		static bool bSaidSo = false;
		UE_CLOG(!bSaidSo, LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] residency: asset manager not ready at %s - retrying every frame until it is."),
			Moment);
		bSaidSo = true;
		return;
	}

	FStreamableAsyncLoadParams Params;
	Params.TargetsToStream.Reserve(GFPMResidencyNumIcons);
	for (const TCHAR* const Path : GFPMResidencyIconPaths)
	{
		// Built through FString on purpose: FSoftObjectPath's character-pointer constructors are
		// per-encoding while the FString one is unconditional, so this cannot be broken by a TCHAR-width
		// change on any target.
		Params.TargetsToStream.Emplace(FString(Path));
	}
	Params.OnComplete = FStreamableDelegateWithHandle::CreateRaw(this, &FFPMAssetResidency::OnIconsLoaded);

	// Default priority, deliberately. Nothing is waiting on these — they only have to be resident before a
	// player first opens a menu, which is seconds away at the earliest. Asking for high priority would
	// contend with loads something IS waiting on, which is the opposite of the point.
	Params.Priority = FStreamableManager::DefaultAsyncLoadPriority;

	PinHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		MoveTemp(Params), TEXT("FPM platform-icon residency"));

	UE_CLOG(!PinHandle.IsValid(), LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] residency: RequestAsyncLoad returned no handle at %s - the platform icons stay unpinned "
		     "and BPW_UserIcon keeps its blocking load."), Moment);
}

void FFPMAssetResidency::OnIconsLoaded(TSharedPtr<FStreamableHandle> CompletedHandle)
{
	if (!CompletedHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] residency: icon load completed with no handle - icons stay unpinned."));
		return;
	}

	/*
	 * Re-resolved BY PATH rather than read out of the handle's array, for two reasons and both matter.
	 *
	 * First, `ResolveObject()` IS the call vanilla's blocking load will make
	 * (`FSoftObjectPath::ResolveObjectInternal` -> `FindObject`, `SoftObjectPath.cpp:886-891`), so this
	 * verifies the exact property the whole fix depends on instead of a proxy for it. If this resolves,
	 * `LoadSynchronous`'s `Get()` will resolve, and the blocking branch is unreachable. That is the fix,
	 * checked rather than assumed.
	 *
	 * Second, it can NAME the path that failed. Reading `GetLoadedAssets()` would leave you working out
	 * which of four nulls is which by array position.
	 */
	Resolved = 0;
	for (const TCHAR* const Path : GFPMResidencyIconPaths)
	{
		if (FSoftObjectPath(FString(Path)).ResolveObject() != nullptr)
		{
			++Resolved;
		}
		else
		{
			// A game update renaming or moving an icon lands here. That icon simply keeps vanilla's blocking
			// load — nothing breaks — but it must be visible rather than silent.
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] residency: '%s' did not resolve after its async load - that icon keeps "
				     "BPW_UserIcon's blocking load. Renamed by a game update?"), Path);
		}
	}

	if (FPMDiag::IsOn(FPMDiag::EChannel::Residency))
	{
		/*
		 * ★ THE BOOT CHECK, FALSIFIABLE IN BOTH DIRECTIONS: the widget's own prints must REMAIN (it still
		 * runs) while the same-frame FlushAsyncLoading lines must be GONE. If the prints vanish too,
		 * something was suppressed that should not have been and this fix is wrong rather than working.
		 *
		 * ⚠ THE WIDGET'S BRACKETED TOKEN IS DELIBERATELY NOT WRITTEN HERE — 2026-08-09. The first version
		 * spelled it out, so this line MATCHED EVERY GREP FOR THE WIDGET'S PRINTS and inflated the count by
		 * one per session. I contaminated the measurement with the line describing the measurement, and
		 * then read the contaminated count. An instrument must not appear in its own results.
		 */
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] residency: %d/%d vanilla platform icons pinned - the user-icon widget's "
			     "LoadAsset_Blocking now finds them resident. Its own avatar prints should REMAIN; the "
			     "same-frame FlushAsyncLoading lines should be GONE."),
			Resolved, GFPMResidencyNumIcons);
	}
}

void FFPMAssetResidency::Disarm()
{
	// The retry ticker outlives nothing. If we are torn down before the asset manager ever came up, this
	// is the only thing holding a raw `this` into the core ticker.
	if (RetryHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RetryHandle);
		RetryHandle.Reset();
	}

	if (PinHandle.IsValid())
	{
		// CancelHandle, not ReleaseHandle. ReleaseHandle defers until after completion when a load is still
		// in flight, which would aim the completion delegate at an object that is going away. CancelHandle
		// is safe on an already-completed handle — it takes the completed branch and just releases.
		PinHandle->CancelHandle();
		PinHandle.Reset();
	}

	// Zero residue. The last reference goes with the handle, so the icons become collectable again exactly
	// as they are in vanilla. Nothing was ever written anywhere, so there is nothing else to undo — and that
	// is by design, because a removed mod cannot run cleanup code.
	Resolved = 0;
}
