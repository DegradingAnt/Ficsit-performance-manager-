// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ M-DETECT TRAP 3 - "WIDGET BINDINGS THAT TICK FOREVER" (design §9.3, item 3). A persistent HUD
 * readout paying per-frame cost: a UMG property binding or a Blueprint Tick event on a widget that
 * never leaves the viewport.
 *
 * ══ WHAT THIS FILE READ UNTIL 2026-08-15, AND WHY IT FLAGGED EVERY WIDGET IN THE GAME ══
 *
 * Ant's overlay printed:
 *
 *     trap 3 (widget tick): 5335 Auto instance(s) across 403 class(es) of 5335
 *
 * 5335 of 5335. A filter that flags everything cannot tell a healthy HUD from a broken one. It is the
 * same defect as a filter that flags nothing, and it had been reporting confidently for days.
 *
 * THE CAUSE: it read `UUserWidget::GetDesiredTickFrequency()`, which is a plain getter
 * (`UserWidget.h:298`) for the `TickFrequency` UPROPERTY (`UserWidget.h:1705-1712`). That field is
 * `EditDefaultsOnly`, and its own doc reads *"This widget is allowed to tick... Uncheck this for
 * performance reasons only"*. It is an OPT-OUT PERMISSION SWITCH set by a human in the class
 * defaults, and the constructor initialises it to `Auto` for every widget ever made
 * (`UserWidget.cpp:92`). Reading it asks "was this widget ALLOWED to tick", never "does it tick".
 * Almost nobody unchecks it, so almost everything reads `Auto`.
 *
 * ⚠ THE COMMENT THAT USED TO SIT HERE IS WHY THE NUMBER WAS TRUSTED. It claimed `Auto` was "the UMG
 * COMPILER's decision, baked onto the class the moment the widget blueprint compiles". Nothing
 * compiles it. A comment is a claim, and that one was false.
 *
 * ══ THE SIGNAL NOW: THE ENGINE'S OWN COMPUTED VERDICT, PER INSTANCE ══
 *
 * `UUserWidget::UpdateCanTick()` (`UserWidget.cpp:2108-2141`) is where the engine decides. Only when
 * `TickFrequency == Auto` does it go on to test `ClassRequiresNativeTick()`,
 * `bHasScriptImplementedTick`, latent actions registered for this object, active animations, queued
 * animation transitions, and any extension whose `RequiresTick()` is true. It then pushes the answer
 * onto the Slate widget with `SObjectWidget::SetCanTick()`.
 *
 * That answer is public and PER INSTANCE, which the authored field never was:
 * `UWidget::GetCachedWidget()` (`Widget.h:837`) hands back the `SObjectWidget`, and
 * `SWidget::GetCanTick()` (`SWidget.h:663`) reads the `NeedsTick` flag it was given.
 * `SObjectWidget::Construct` clears that flag and immediately calls `UpdateCanTick()`
 * (`SObjectWidget.cpp:22-31`), and around twenty further call sites recompute it whenever anything
 * relevant changes, so the flag is a live answer and not a stale one.
 *
 * ★ FOUR OUTCOMES, NOT TWO, AND THE CENSUS PRINTS ALL FOUR. `NotConstructed` is a COVERAGE HOLE
 * rather than a verdict: a widget with no Slate widget yet has had no decision made about it, and
 * folding those into "not ticking" would be reporting an answer nobody has given. `OptedOut` is
 * printed for its own sake, because it is the number that shows how little the old reading excluded.
 *
 * ★ WHY THE REPORT STILL DOES NOT CLAIM "BINDING". Six different things set the flag. Narrowing the
 * wording to match the trap's name would overclaim what one bit can prove, exactly as the old wording
 * overclaimed what an authored default could prove. The description says what was read.
 *
 * ★ CENSUS BY LIVE INSTANCE, NOT BY CLASS. One ticking widget class instantiated once per inventory
 * slot is the precise "many small persistent HUD readouts" shape the trap names, so the INSTANCE
 * COUNT is the cost multiplier. Same reasoning as trap 1's Blueprint-tick census.
 *
 * ★ THE LIVENESS QUESTION: WHAT CONCRETE INPUT MAKES THIS REPORT A DIFFERENT NUMBER? Any widget
 * whose `UpdateCanTick()` computed true: a Blueprint Tick event, a playing animation, a pending
 * latent action, a native C++ base without the `DisableNativeTick` metadata. Close the menu that owns
 * one and the count falls at the next run. The reading it replaced had no such input: 5335 of 5335
 * could not fall.
 *
 * VIEWER ONLY: reads two engine getters and TObjectIterator, and reports through FFPMDetectorRegistry.
 * No hook, no cvar write, no ini.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMWidgetTickDetector final : public IFPMFix
{
public:
	static FFPMWidgetTickDetector& Get();

	virtual const TCHAR* Name() const override { return TEXT("widget-tick-detector"); }

	/** NeverOnDedicatedServer - UMG widgets do not exist without a local viewport; a dedicated
	 *  server has none, so this would read a permanent, meaningless zero there. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Detect; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;

	/** `FPM.Detect.WidgetTick` - census live widgets now. */
	static void RunNow();

	/**
	 * ★ THE LIVENESS PROOF, RUN AT Arm() EVERY BOOT. Three assertions, and none of them can pass
	 * without the read path working.
	 *
	 * TWO ON THE SLATE READ PATH, against a throwaway `SBox` whose tick flag is forced both ways.
	 * `SetCanTick` writes the same `EWidgetUpdateFlags::NeedsTick` bit that `UUserWidget::UpdateCanTick`
	 * writes through `SObjectWidget::SetCanTick`, and `GetCanTick` is the accessor the census reads. The
	 * probe is not registered with an invalidation root, so the write is a plain flag assignment with no
	 * side effect on anything on screen (`WidgetProxy.cpp:380-394` guards on `IsValid(Widget)`).
	 *
	 * ONE ON THE CLASSIFIER, and it is the one that would have caught the bug this file was rebuilt for.
	 * The base `UUserWidget` CDO carries `TickFrequency == Auto` and has NO Slate widget, so the
	 * classifier must return `NotConstructed`. A classifier that went back to reading the authored enum
	 * would return "ticking" for that CDO and fail the boot line.
	 *
	 * ⚠ THE SELF-TEST THIS REPLACES WAS ITSELF DEAD. It asserted that the base `UUserWidget` CDO reads
	 * `EWidgetTickFrequency::Never`. The constructor sets `Auto` (`UserWidget.cpp:92`), so that
	 * known-negative was false on every boot, and the line printed FAILED forever while the census beside
	 * it printed 100%.
	 *
	 * @return true if all three assertions held.
	 */
	static bool SelfTest();
};
