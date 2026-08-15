// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ M-DETECT TRAP 3 - "WIDGET BINDINGS THAT TICK FOREVER" (design §9.3, item 3). A persistent HUD
 * readout paying per-frame cost - a UMG property binding or a Blueprint Tick event on a widget
 * that never leaves the viewport.
 *
 * ★ THE SIGNAL IS THE ENGINE'S OWN COMPILED VERDICT, NOT A RE-DERIVED HEURISTIC.
 * `UUserWidget::GetDesiredTickFrequency()` (`Blueprint/UserWidget.h:298`, public) returns
 * `EWidgetTickFrequency` - `Never` or `Auto` (`:119-130`). The header's own comment on `Auto` is
 * explicit about what sets it: *"will tick if a blueprint tick function is implemented, any
 * latent actions are found or animations need to play... If the widget inherits from something
 * other than UserWidget it will also tick"*. That is the UMG COMPILER's decision, baked onto the
 * class the moment the widget blueprint compiles - reading it is not reflection or a guess, it is
 * asking the engine the exact question this trap is about.
 *
 * ★ WHY `Auto` IS REPORTED HONESTLY AS BROADER THAN "BINDINGS", NOT NARROWED TO MATCH THE TRAP'S
 * NAME. Property bindings are one of several things that can set `Auto`; a widget with a genuine
 * animation or a native C++ base also reads `Auto`. Narrowing the report to claim "this widget has
 * a binding" would overclaim what a single enum value can prove. The description says what was
 * actually read.
 *
 * ★ CENSUS BY LIVE INSTANCE, NOT BY CLASS. A single `Auto`-ticking widget class instantiated once
 * per inventory slot is the exact "many small persistent HUD readouts" shape the trap names - the
 * INSTANCE COUNT is the cost multiplier, same reasoning as trap 1's Blueprint-tick census.
 *
 * ★ THE LIVENESS QUESTION: WHAT CONCRETE INPUT MAKES THIS REPORT NON-ZERO? Any live, non-vanilla
 * `UUserWidget` instance whose `GetDesiredTickFrequency() == Auto`. See `SelfTest` for how the
 * READ PATH itself (not a classifier of this file's own invention) is proven.
 *
 * VIEWER ONLY: reads a widget property and TObjectIterator, reports through FFPMDetectorRegistry.
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
	 * ★ THE LIVENESS PROOF. This file reads an engine-computed field rather than classifying
	 * anything itself, so the proof is about the READ PATH: known-negative is
	 * `UUserWidget::StaticClass()`'s own CDO - the un-subclassed base implements no Tick event, no
	 * bindings, no animation, and inherits directly from UserWidget (not "something other"), so
	 * its `GetDesiredTickFrequency()` must be `Never`. A widget base class that read `Auto` would
	 * mean the read path is broken, not that anything real ticks.
	 *
	 * Known-positive is sought the same way trap 1 seeks one: walk every currently-loaded
	 * `UUserWidget` subclass's CDO for the first one that reads `Auto`. None being loaded yet (real
	 * at `Arm()` time, which runs from `StartupModule` before any UI exists) is printed as
	 * UNVERIFIED coverage rather than folded into a bare pass - `RunNow`'s own boot, after a world
	 * has loaded and the HUD exists, is where a real positive is expected.
	 *
	 * @return true if the known-negative held.
	 */
	static bool SelfTest();
};
