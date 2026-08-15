// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ M-DETECT TRAP 1 - "BLUEPRINT CONVEYOR GRAB LOGIC" (design §9.3, item 1 of the four community
 * traps; inbox §37/§41's canonical tutorial). Blueprint VM cost per item per belt tick, paid by
 * any mod-authored buildable that implements its factory tick in a Blueprint graph instead of
 * C++. Detector: per-class census of mod-owned Blueprint buildables that override the tick event.
 *
 * ★ THE SIGNAL, RE-DERIVED FROM THE REAL HEADER RATHER THAN GUESSED. `AFGBuildable` declares
 * `Factory_Tick(float)` in C++ (`FGBuildable.h:288`) and a `BlueprintImplementableEvent` sibling,
 * `Factory_ReceiveTick` (`:291-292`), documented as "Blueprint version of Factory_Tick" - PUBLIC,
 * so it is reachable via reflection without needing a friend or an AccessTransformer.
 * `AFGBuildable::Factory_Tick` itself calls `Factory_ReceiveTick(dt)` ONLY when
 * `mHasBlueprintFactoryTick` is set (`FGBuildable.cpp:2429-2432`) - that field IS the ground
 * truth, but it is `private` (`FGBuildable.h:1023` section, field at `:1053`) and never assigned
 * anywhere in FactoryGame's own `.cpp` (grepped `mHasBlueprintFactoryTick\s*=` across the whole
 * module - zero hits), so it is baked at Blueprint-compile time by machinery this mod cannot
 * reach and is not readable from outside the class either way.
 *
 * The reachable equivalent: `Class->FindFunctionByName("Factory_ReceiveTick")` returns the
 * UFunction that answers a call to the event, and its `GetOuter()` is the UClass that DEFINES
 * that function. On `AFGBuildable` itself the outer is `AFGBuildable::StaticClass()` - nobody has
 * overridden it there, because the base only DECLARES the event. Any class below it whose outer
 * differs has added its own implementation - exactly the condition
 * `AFGBuildable::Factory_Tick` would find true at runtime and dispatch into.
 *
 * ★ WHY THIS IS A CENSUS, NOT A TIMED COST. There is no cheap, safe way to wrap Blueprint VM
 * execution time per class from outside the engine without a profiler hook this project does not
 * have (and should not add - a timing wrapper around every buildable's tick is itself the kind of
 * per-frame cost this mod exists to avoid). What IS measurable, honestly: WHICH mod-owned classes
 * pay the Blueprint-dispatch cost at all, and HOW MANY LIVE INSTANCES of each are on the save
 * right now - instance count is the direct multiplier on "per item per belt tick" cost the trap
 * names, even without a millisecond figure attached to it. The report says exactly this rather
 * than implying a cost figure it cannot produce.
 *
 * ★ THE LIVENESS QUESTION: WHAT CONCRETE INPUT MAKES THIS REPORT NON-ZERO? Any loaded
 * `AFGBuildable` subclass that is (a) Blueprint-generated (`Cast<UBlueprintGeneratedClass>`
 * succeeds) and (b) overrides `Factory_ReceiveTick` per the outer-comparison above, with at least
 * one live actor instance. A save with zero mod-authored Blueprint buildables implementing their
 * own tick is a REAL zero, and the report's coverage line says how many Blueprint-generated
 * `AFGBuildable` subclasses were even found loaded, so that zero is legible as "checked, none
 * found" rather than "the walk never ran".
 *
 * VIEWER ONLY: reads class reflection data and TActorIterator, reports through
 * FFPMDetectorRegistry. No hook, no cvar write, no ini.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMConveyorGrabDetector final : public IFPMFix
{
public:
	static FFPMConveyorGrabDetector& Get();

	virtual const TCHAR* Name() const override { return TEXT("conveyor-grab-detector"); }

	/** Any - a class census over TActorIterator has nothing renderer-specific in it, and the
	 *  dedicated server runs the same Blueprint-tick dispatch the client does. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** UnknownCause: this NAMES a cost carrier, it does not diagnose or fix one. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Detect; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;

	/**
	 * Removes the deferred one-shot ticker. Without this, FPMFixes::DisarmAll() would report this
	 * fix disarmed while the ticker kept firing into a module that thinks it has torn down - the
	 * exact inventory-lying shape FPMHookLedger and the structure gate both exist to catch (see
	 * FPMDistanceFieldAudit::Disarm for the same fix on the same class of bug).
	 */
	virtual void Disarm() override;

	/** `FPM.Detect.ConveyorGrab` - runs the census now and reports through the registry. */
	static void RunNow(UWorld* World);

	/**
	 * The classifier, pulled out so `SelfTest` can drive it directly. True when `Class` (or
	 * anything below `BaseClass` in its hierarchy) overrides `EventFunctionName`, which
	 * `BaseClass` only declares.
	 */
	static bool ClassOverridesEvent(const UClass* Class, const UClass* BaseClass, FName EventFunctionName);

	/**
	 * ★ THE LIVENESS PROOF FOR THE CLASSIFIER MECHANISM. `Factory_ReceiveTick` cannot be given a
	 * synthetic known-positive without shipping a probe Blueprint asset (rejected - this is a
	 * native-code mod and a self-test asset is exactly the content dependency the design avoids).
	 * So the mechanism itself - outer-comparison detects an override and does not false-positive
	 * on the undecorated base - is proven against `AFGBuildable::StaticClass()` as the KNOWN
	 * NEGATIVE (must report false: the base does not override its own declaration) and, if the
	 * live class tree currently holds ANY class that overrides ANY function declared no higher
	 * than `AFGBuildable` (walked generically, not restricted to Factory_ReceiveTick), that
	 * confirms the mechanism CAN detect a real positive on THIS build. Absence of such a class is
	 * reported as what it is - the mechanism's positive path is UNVERIFIED this boot - never
	 * folded into a bare PASS.
	 *
	 * @return true if the known-negative check passed AND (a positive was found demonstrating the
	 *         mechanism works, OR none exists and that absence is itself printed, not hidden).
	 */
	static bool SelfTest();
};
