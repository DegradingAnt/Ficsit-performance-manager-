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
	const TCHAR* const GFPMResidencyPaths[] =
	{
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_Steam_128.TXUI_Steam_128"),
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_Epic_128.TXUI_Epic_128"),
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_XBOX_128.TXUI_XBOX_128"),
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_PlayStation_128.TXUI_PlayStation_128"),

		/*
		 * ★ NOT AN ICON, AND THE MOST EXPENSIVE ENTRY IN THIS LIST. Added 2026-08-11 from Ant's own
		 * client log — the asset is named by the hitch meter, not guessed:
		 *
		 *   HITCH 327.2 ms | GAME-THREAD BOUND | 2 SYNC LOAD(S),
		 *     last='/Game/FactoryGame/Settings/OptionsMenu/UserInterface/US_ShowCreaturePerceptionIndicators'
		 *
		 * Twelve of those in one 90-minute session, 310-336 ms each, at irregular intervals - which fits a
		 * creature-perception indicator being created when something notices the player, not a timer. It
		 * is a `UFGUserSetting` reached through a soft reference, so nothing keeps it resident and every
		 * query pays a blocking load. Exactly the shape the platform icons above are here for.
		 *
		 * ⚠ THE FIX IS EVIDENCE-BASED, THE CURE IS NOT YET PROVEN. The asset is definitely being sync
		 * loaded and definitely costs ~320 ms. Whether PINNING it removes the hitch depends on the load
		 * being the whole cost rather than one visible step of a larger stall, and that needs a boot to
		 * settle. The hitch meter answers it directly: if these spikes survive with the asset pinned, the
		 * sync load was a symptom and this entry should come back out rather than stay as cargo.
		 *
		 * ⚠ The log names ONE of TWO sync loads in that span ("last="). The other is unidentified, so a
		 * partial improvement is a plausible outcome and must not be read as the fix failing.
		 */
		TEXT("/Game/FactoryGame/Settings/OptionsMenu/UserInterface/US_ShowCreaturePerceptionIndicators.US_ShowCreaturePerceptionIndicators"),
	};

	constexpr int32 GFPMResidencyNumPaths = static_cast<int32>(UE_ARRAY_COUNT(GFPMResidencyPaths));
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
	Params.TargetsToStream.Reserve(GFPMResidencyNumPaths);
	for (const TCHAR* const Path : GFPMResidencyPaths)
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
		TEXT("[FPM] residency: RequestAsyncLoad returned no handle at %s - the pinned set stays unpinned "
		     "and BPW_UserIcon keeps its blocking load."), Moment);
}

void FFPMAssetResidency::OnIconsLoaded(TSharedPtr<FStreamableHandle> CompletedHandle)
{
	if (!CompletedHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] residency: pin load completed with no handle - the set stays unpinned."));
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
	for (const TCHAR* const Path : GFPMResidencyPaths)
	{
		if (FSoftObjectPath(FString(Path)).ResolveObject() != nullptr)
		{
			++Resolved;
		}
		else
		{
			// A game update renaming or moving one lands here. That asset simply keeps vanilla's blocking
			// load — nothing breaks — but it must be visible rather than silent.
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] residency: '%s' did not resolve after its async load - that asset keeps "
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
			TEXT("[FPM] residency: %d/%d vanilla assets pinned (4 platform icons + the creature-perception setting) - the user-icon widget's "
			     "LoadAsset_Blocking now finds them resident. Its own avatar prints should REMAIN; the "
			     "same-frame FlushAsyncLoading lines should be GONE."),
			Resolved, GFPMResidencyNumPaths);
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

	// Zero residue. The last reference goes with the handle, so the pinned assets become collectable again exactly
	// as they are in vanilla. Nothing was ever written anywhere, so there is nothing else to undo — and that
	// is by design, because a removed mod cannot run cleanup code.
	Resolved = 0;
}
