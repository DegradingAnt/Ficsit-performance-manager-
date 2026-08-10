// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ HUD HOOK GUARD — removes ONE crashing blueprint-hook descriptor and lets everything else install.
 * Design P3.5.
 *
 * THE CRASH: a hard assert on every death and every vehicle exit.
 *     Widget_PlayerHUD_C:ExecuteUbergraph_Widget_PlayerHUD:10000000C
 *     Script call stack: Widget_PlayerHUD_C:Construct
 *     -> execAddMulticastDelegate -> execAssert -> Fatal
 * A blueprint hook injects into the vanilla player HUD's `Construct`, and the injected code binds a
 * multicast delegate there. That is what dies. Observed owner: KPrivateCodeLib's
 * `/KPrivateCodeLib/Hooks/Hook_PlayerWidget.Hook_PlayerWidget_C`.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ THIS GUARD'S OWN HISTORY IS THE REASON IT IS SHAPED THIS WAY. It was too broad TWICE, and Ant was
 * on the receiving end of both.
 *
 *   v1 — cancelled EVERY blueprint hook targeting Widget_PlayerHUD. Any other mod hooking that widget
 *        safely lost its UI, with no way for its author to find out why.
 *   v2 — narrowed to KPrivateCodeLib's asset, then cancelled that asset ENTIRELY. But KPrivateCodeLib
 *        is the library under KAPI / KBFL / KUI / KBlueprintDesignPlus, so one `Cancel()` removed the
 *        HUD contribution of a whole family of mods. Ant: *"some modded UI doesnt close on escape and
 *        parts of their UI dosnt exist."*
 *   v3 — the version ported here. THE CRASH IS ONE DESCRIPTOR, NOT ONE ASSET. A hook asset carries many
 *        descriptors; only the one injecting into `Construct` asserts. Strip that descriptor and let
 *        registration proceed, so the asset installs and its other hooks work.
 *
 * Her standing rule is the measure: *"we never block or hinder UI or mod funtions if they are not TRULY
 * and BEYOND broken. we fix things so all things work as intended."* A hard crash on every death clears
 * the "truly broken" bar — but only for the descriptor that actually causes it.
 *
 * ⚠ SO IF ANT STILL HAS MISSING MOD UI, THIS IS PROBABLY NOT IT. v3 shipped on the old mod in 0.58.72
 * and the logs show it stripping rather than cancelling. Her current complaint needs its own origin
 * hunt rather than being assumed to be this. Said here so the next reader does not close that thread
 * by pointing at this file.
 *
 * ⚠ IT MUTATES ANOTHER MOD'S ASSET IN MEMORY, and that is worth saying out loud rather than burying. It
 * is strictly less invasive than cancelling the registration, which is what it replaces. It is confined
 * to descriptors targeting `Widget_PlayerHUD::Construct`, it runs BEFORE registration so nothing is left
 * half-installed, and the log names exactly what was removed so the other author can see what we took.
 *
 * ⚠ IF STRIPPING LEAVES NOTHING, CANCEL INSTEAD. An asset whose only descriptor is the crashing one has
 * nothing left to register, and letting an empty registration through would be a silent no-op dressed
 * up as success.
 */
class FFPMHudHookGuard final : public IFPMFix
{
public:
	static FFPMHudHookGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("hud-hook-guard"); }

	/**
	 * ★ THE CONTRACT'S OWN SIDE GATE, not a hand-rolled `IsRunningDedicatedServer()` early-return.
	 *
	 * A dedicated server has no player HUD, so this can only ever be inert there — while still installing
	 * a hook into a separately compiled Linux binary. That is pure risk for zero benefit, and the old
	 * mod's server FAILED TO BOOT while carrying an earlier version of it. Declaring the side means
	 * `FPMFixes::Arm` skips it AND logs that it skipped, so "not in the inventory" and "gated off" stay
	 * distinguishable in a server log.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** Guard: the crashing injection belongs to another mod and the assert belongs to the engine's
	 *  blueprint VM. We remove the one descriptor; we do not own either side. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::HudGuard; }

	virtual void Arm() override;

	/** Registrations seen that touch the HUD · descriptors stripped · assets allowed through untouched ·
	 *  whole assets cancelled. The last number is the one to watch: it should be ZERO, and a non-zero
	 *  value means some mod lost more than one descriptor. */
	static void GetCounts(int32& OutSeen, int32& OutStripped, int32& OutAllowed, int32& OutCancelled);

	/**
	 * Removes the hook.
	 *
	 * ⚠ Without this, `FPMFixes::DisarmAll()` reports this fix disarmed while its handler keeps
	 * running. Near-harmless at process exit, which is the only place DisarmAll has ever been called
	 * from and why the omission survived; it is what blocked P4.2's master OFF switch.
	 */
	virtual void Disarm() override;

private:
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle RegisterBlueprintHookHandle;
};
