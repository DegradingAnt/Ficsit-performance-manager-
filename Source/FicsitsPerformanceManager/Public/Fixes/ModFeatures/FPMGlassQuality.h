// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMDiag.h"
#include "Core/FPMFixContract.h"

/**
 * ★ BETTER GLASS — LUMEN FRONT-LAYER TRANSLUCENCY REFLECTIONS, HELD ON, WITH NO INI AND NO POLL.
 *
 * Ant, 2026-08-02: *"glass looks bad always tho"* · *"turn it on by default so windows dont look like
 * shit at load"*. And carrying it into FPM2, 2026-08-10: *"the glass should live and die with the mod.
 * make it so the mod turns it on and keeps it on. when the govnour is in we can make it do the glass but
 * for now the main mod will just have a 'better glass' toggle."*
 *
 * ══ TWO CVARS, NOT ONE. THAT WAS THE ORIGINAL BUG AND IT IS WHY THIS EXISTS. ══
 *
 * The engine gate, read this session at `LumenFrontLayerTranslucency.cpp:55-58`:
 *
 *     return (View.FinalPostProcessSettings.LumenFrontLayerTranslucencyReflections
 *             || GLumenFrontLayerTranslucencyReflectionsEnabled)
 *         && GLumenFrontLayerTranslucencyReflectionsAllowed != 0
 *         && View.Family->EngineShowFlags.LumenReflections;
 *
 * `.Enable` feeds the first clause, `.Allow` the second, and the second is an AND. `BaseScalability.ini`
 * decides `.Allow` per reflection-quality level, read this session:
 *
 *     :384 [ReflectionQuality@2]     :393 FrontLayer.Allow=0   :394 FrontLayer.Enable=0
 *     :396 [ReflectionQuality@3]     :405 FrontLayer.Allow=1   :406 FrontLayer.Enable=0
 *     :408 [ReflectionQuality@Cine]  :417 FrontLayer.Allow=1   :418 FrontLayer.Enable=1
 *
 * At `sg.ReflectionQuality=2` — High, the common setting — `.Allow` is 0, so writing `.Enable=1` alone
 * is gated out and does exactly nothing. FPM1 wrote only `.Enable`, and so the glass looked wrong at
 * every quality level, for as long as the feature had existed. Both keys, or neither.
 *
 * ⚠ `.Allow` IS ALSO THE CORRECT ESCAPE HATCH. If front-layer reflections ever prove too expensive on a
 * weaker card, gate on `.Allow`. Never on `.Enable` alone — that is the same mistake in the other
 * direction.
 *
 * ══ NO INI. THIS IS THE ONE REAL DIFFERENCE FROM FPM1, AND IT COSTS SOMETHING. ══
 *
 * FPM1 shipped this as BOTH an `Engine.ini` `[SystemSettings]` line and a runtime write, because an ini
 * is read before mods exist and "at load" was the requirement. FPM2 is ZERO RESIDUE — an uninstall must
 * leave nothing — so the ini half is gone.
 *
 * **What that costs, stated rather than glossed: the loading screen and the first frames after it can
 * render with the vanilla glass, because FPM cannot write a cvar before it is loaded.** The fix is one
 * `Hold` at StartupModule, which is as early as a mod can act. If that turns out to be visible to her in
 * practice, the answer is not to reintroduce the ini — it is to find an earlier arm point.
 *
 * ══ WHY THERE IS NO 2-SECOND RE-ASSERT LOOP ══
 *
 * Ant asked directly: *"but if it loops every 2 s then wont it lag the main thread?"* It would, and it
 * does not, because the loop is not the mechanism.
 *
 * Both cvars are `ECVF_Scalability` (`LumenFrontLayerTranslucency.cpp:23` and `:40`), so every
 * scalability re-apply tries to write them. But the console manager refuses a write whose priority is
 * below the one in force — `FConsoleVariableBase::CanChange` (`ConsoleManager.cpp:267-272`) is literally
 * `NewPri >= OldPri`, and it LOGS the refusal. `FPMCVarWriter` holds at `ECVF_SetByPluginHighPriority`
 * (0x07). Scalability writes at `ECVF_SetByScalability` (0x01). 0x01 loses.
 *
 * ⚠ **SO EXPECT `LogConsoleManager: Warning` LINES NAMING THESE TWO CVARS, AND THEY ARE OURS.** Every
 * settings-menu Apply will produce two of them, saying the `SetByScalability` write was ignored. That is
 * the engine correctly reporting that a deliberate override won. It is noise FPM causes, so it is named
 * here and in the arm line rather than left for her to find and wonder about.
 *
 * ══ AND WHY THERE IS STILL A CHECK ══
 *
 * The paragraph above is an ARGUMENT, and the mod has been wrong before about a lever it reasoned its
 * way to. `FFPMTexturePoolGuard` holds a cvar through the same writer and still logs
 * "REPAIRED a scalability clobber", which is either a different priority path or a stale belief — and
 * either way it means the argument is not settled by reading.
 *
 * So this hooks `UFGGameUserSettings::ApplyNonResolutionSettings` (`FGGameUserSettings.h:112`) with an
 * _AFTER handler and READS BOTH VALUES BACK. Event-driven, so it costs nothing between settings changes.
 * If they held, it counts a verification. If either dropped, it re-asserts and counts a REPAIR, loudly.
 *
 * ★ THAT COUNTER IS THE LIVENESS PROOF AND IT SETTLES THE QUESTION IN ONE BOOT:
 *   - verifications 0            -> the check never ran. The report says DEAD, not "all clear".
 *   - verifications > 0, repairs 0 -> the priority argument holds and the re-assert can be deleted.
 *   - repairs > 0                -> the argument is WRONG, the re-assert is load-bearing, and the log
 *                                   names which cvar lost so the real stomp point can be found.
 *
 * The input that produces a non-zero is nameable and cheap: open the settings menu and press Apply.
 *
 * ══ SCOPE ══
 *
 * CLIENT ONLY. A dedicated server renders nothing, so `Side()` refuses there.
 *
 * ZERO RESIDUE: two holds through `FPMCVarWriter` on a `Module` lease, released at ShutdownModule via
 * the engine-native `Unset(priority, Tag)` path. No ini is written. Neither cvar is US_*-backed, so
 * clause 6 does not apply and nothing can be serialised into her `GameUserSettings.ini`.
 *
 * `FPM.Glass.Enable 0` turns it off at runtime and releases both holds, which returns the cvars to
 * whatever the game itself last set. `FPM.Glass.Report` prints the state and the counters.
 *
 * ⚠ WHEN THE GOVERNOR ARRIVES it takes this over, per Ant's ruling. Until then this is a plain toggle
 * that is ON, and it deliberately has no quality ladder of its own — a second thing that moves
 * reflection quality would fight the governor the day it lands.
 */
class FFPMGlassQuality final : public IFPMFix
{
public:
	static FFPMGlassQuality& Get();

	virtual const TCHAR* Name() const override { return TEXT("glass-quality"); }

	/** A dedicated server draws no glass. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * The cause is named with a receipt: `BaseScalability.ini:393` sets `FrontLayer.Allow=0` at
	 * `sg.ReflectionQuality@2`, and the engine gate at `LumenFrontLayerTranslucency.cpp:55-58` ANDs it.
	 * That is why the glass looked wrong, and it is why one cvar was never going to be enough.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::GlassQuality; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** `FPM.Glass.Report` — both cvar values, both holds, and the verification counters. */
	static void LogReport(class FOutputDevice* Ar = nullptr);

	/**
	 * Apply the current `FPM.Glass.Enable` value: hold both cvars, or release both.
	 *
	 * Public because the cvar's own change callback calls it, so toggling in the console takes effect
	 * immediately rather than at the next boot.
	 */
	static void ApplyFromToggle(const TCHAR* Reason);

private:
	FDelegateHandle ApplyHookHandle;
};
