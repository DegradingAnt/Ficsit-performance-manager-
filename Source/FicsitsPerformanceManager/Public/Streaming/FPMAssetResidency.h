// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

#include "Core/FPMFixContract.h"

struct FStreamableHandle;

/**
 * ASSET RESIDENCY — keeps four tiny VANILLA textures loaded, so that vanilla's own BLOCKING load of them
 * finds nothing left to do.
 *
 * ★ THIS FIX WAS ROOT-CAUSED ON 2026-08-02 AND THEN SAT UNBUILT FOR A WEEK. It was recovered by the
 * 2026-08-09 scratchpad audit (`SCRATCHPAD-AUDIT-2026-08-09.md`, top row of LIVE AND UNSHIPPED) from
 * `patches-2026-08-02/fpm-patch-asyncload.md`. Every claim below was re-verified at bytes before this file
 * was written — a week-old note is a hypothesis until it is re-checked.
 *
 * THE BUG. `Widget_PlayerList_Item::OnListItemObjectSet` calls `BPW_UserIcon::SetServiceIcon`
 * (`Widget_PlayerList_Item.cpp:489-511` in the FModel decompile), which calls
 * `UKismetSystemLibrary::LoadAsset_Blocking` (`BPW_UserIcon.cpp:308`) on a SOFT reference to one of four
 * platform-service icon textures. The map is a `SoftObjectProperty` (`BPW_UserIcon.json:2827`) and its four
 * defaults are, verbatim from the export (`BPW_UserIcon.json:18-47`, re-read 2026-08-09):
 *
 *     Steam -> TXUI_Steam_128 · Epic -> TXUI_Epic_128 · Xbox -> TXUI_XBOX_128 · PSN -> TXUI_PlayStation_128
 *
 * Nothing in the game holds a HARD reference to those four, so every time a player-list entry binds, the
 * game thread loads the package synchronously. Measured: **17 of 26 `FlushAsyncLoading` lines in one
 * 30-minute session share a FRAME NUMBER** with the `[BPW_UserIcon]` print emitted two statements earlier,
 * and every governor window containing one reported a 383-405 ms worst frame.
 *
 * ★ WHY PINNING IS THE ROOT FIX AND NOT A DODGE. `LoadAsset_Blocking`'s whole body is
 * `Asset.LoadSynchronous()` (`KismetSystemLibrary.cpp:1442-1445`), and `LoadSynchronous` is
 *
 *     UObject* Asset = Get();
 *     if (Asset == nullptr && !IsNull()) { ToSoftObjectPath().TryLoad(); Asset = Get(); }
 *
 * (`SoftObjectPtr.h:82-93`). `Get()` ends in `FSoftObjectPath::ResolveObjectInternal`, which is a plain
 * `FindObject` — **a lookup, never a load** (`SoftObjectPath.cpp:886-891`). A resident texture therefore
 * skips the blocking branch outright. That single `if` is the entire mechanism: the disk work is removed at
 * its source, the widget still runs, and it still gets the same texture object. Nothing is suppressed and
 * no behaviour changes.
 *
 * ⚠ WHY NOT A HOOK, STATED SO IT IS NOT RE-PROPOSED. `LoadAsset_Blocking` is a FOUR-LINE function — exactly
 * the tiny-target shape that corrupted under funchook in the 0.3.0 `IsDynamicBase` crash — and it is global
 * to every blueprint in the game and every other mod. **This fix installs no hook at all**, which is why it
 * will not appear in the hook ledger.
 *
 * ⚠ AND WHY NOT THE OTHER TWO OBVIOUS ROUTES. Suppressing `SetServiceIcon` or blanking the icon deletes a
 * feature and calls it a fix. Shipping an async replacement for the vanilla widget breaks the standing rule
 * that FPM ships no game assets, and would break silently on every game patch.
 *
 * ⚠ ONE CORRECTION TO THE SOURCE NOTE, MADE HERE RATHER THAN CARRIED SILENTLY. It calls these "pause/ESC
 * menu" hitches, but the lines it cites as the following context — `UpdateFocusHighlights [mCreateNewGame]`
 * and `Widget_ServerManager NO CDO` — read as MAIN-MENU widgets, not the in-game pause menu. The fix is
 * identical either way (same widget, same soft reference), so nothing here depends on the answer; but the
 * residency is armed for BOTH moments below for exactly that reason, and the label should not be repeated
 * as if it were settled.
 *
 * ⚠ SCOPE IS DELIBERATELY FOUR ASSETS. This is the place to add another asset ONLY when a blocking load of
 * it has been shown in a log. A general "preload useful things" cache is a memory leak with a nicer name.
 *
 * ZERO RESIDUE: no cvar, no ini, no save, no content override — the change writes nothing anywhere. It
 * holds ~350 KB of textures (4 × 128×128 `PF_B8G8R8A8` + mips) for as long as the mod is loaded and drops
 * them on `Disarm`. Uninstalled, it does nothing, because there is nothing left behind to do anything.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMAssetResidency final : public IFPMFix
{
public:
	static FFPMAssetResidency& Get();

	virtual const TCHAR* Name() const override { return TEXT("asset-residency"); }

	/**
	 * A dedicated server builds no player list and no user icon, so the blocking load cannot happen there
	 * and the pinned memory would be pure waste. This clears the enum's stated bar — the widget subsystem
	 * genuinely does not exist on a server — rather than resting on "this feels like a client thing".
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;
	virtual void Disarm() override;

private:
	/**
	 * Idempotent. Called from BOTH `Arm()` and `OnWorldLoad()` because the earliest safe moment is not
	 * knowable from here and guessing it wrong is silent.
	 *
	 * `UAssetManager` is constructed during `UEngine::InitializeObjectReferences`, which may or may not have
	 * run by the time a game-feature module starts up. Rather than assert an ordering, this asks
	 * `UAssetManager::IsInitialized()` (`AssetManager.h:94`) and simply tries again at the next world load
	 * if the answer was no. Both call sites are well before a player can open a menu, which is the only
	 * deadline that matters.
	 */
	void EnsurePinned(const TCHAR* Moment);

	/** Re-resolves by PATH and reports per-asset — see the .cpp for why not out of the handle's array. */
	void OnIconsLoaded(TSharedPtr<FStreamableHandle> CompletedHandle);

	/**
	 * ★ THIS HANDLE IS THE GC REFERENCE, and that is verified rather than assumed:
	 * `struct FStreamableManager : public FGCObject` (`StreamableManager.h:702`) with
	 * `virtual void AddReferencedObjects(FReferenceCollector&) override` (`:873`). While a handle is alive
	 * the manager keeps its targets referenced, so the textures cannot be collected between menu opens —
	 * which is precisely the gap that makes vanilla re-load them every time.
	 *
	 * The source note additionally held a `UPROPERTY` array and called it belt-and-braces. It is not
	 * carried: this class is not a UObject, so that array would need `FGCObject` machinery of its own to
	 * mean anything, and duplicating a reference the engine already holds buys nothing but a second thing
	 * that can rot.
	 */
	TSharedPtr<FStreamableHandle> PinHandle;

	/** Reported, so "pinned nothing" can never look the same as "pinned everything". */
	int32 Resolved = 0;
};
