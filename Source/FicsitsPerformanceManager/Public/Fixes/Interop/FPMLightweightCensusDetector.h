// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ M-DETECT TRAP 4 - "BUILDINGS BUILT AS FULL ACTORS INSTEAD OF LIGHTWEIGHT INSTANCES" (design
 * §9.3, item 4 - named as "the biggest" of the four community traps). Actor tick, replication and
 * component cost per placement, multiplied by however many thousand placements a factory reaches.
 * Design's own instruction: census `AFGBuildable`-derived ACTORS versus lightweight instances per
 * owning class, via `TActorIterator` - "already the house pattern" (already used by
 * `FPMCratesSweep`, `FPMDistanceFieldAudit`, `FPMRailConnectionGuard`, `FPMNavMeshCeiling`).
 *
 * ★ THE LIGHTWEIGHT SIDE, VERIFIED AGAINST THE REAL SUBSYSTEM.
 * `AFGLightweightBuildableSubsystem::GetAllLightweightBuildableInstances()`
 * (`FGLightweightBuildableSubsystem.h:772`, public `FORCEINLINE`) returns
 * `TMap<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>` - the live per-class
 * lightweight population, read directly rather than derived.
 *
 * ★ WHAT THIS DOES NOT CLAIM. There is no public capability flag anywhere in
 * `AFGBuildable`/the lightweight subsystem saying "this class COULD be lightweight but wasn't" -
 * searched, not found. Inventing that judgement from absence would be exactly the overclaiming
 * this project's labelling law forbids. So this reports the RAW counts side by side per class: a
 * mod-owned class with many live actors and zero entries anywhere in the lightweight map, at
 * scale, is the trap's own signature (never uses the lightweight system at all) - the reader
 * draws the conclusion the numbers support, this file does not draw it for them.
 *
 * ★ THE SCALE THRESHOLD EXISTS SO A ONE-OFF UNIQUE BUILDING (a mod's own HUB-equivalent, deliberately
 * never lightweight) IS NOT FLAGGED ALONGSIDE A REPEATED BUILDABLE PLACED THOUSANDS OF TIMES -
 * see `MinActorCountToFlag` below for the number and why.
 *
 * ★ THE LIVENESS QUESTION: WHAT CONCRETE INPUT MAKES THIS REPORT NON-ZERO? A non-vanilla
 * `AFGBuildable` subclass with at least `MinActorCountToFlag` live actors and zero entries in the
 * lightweight instance map for that exact class.
 *
 * VIEWER ONLY: reads TActorIterator and a public subsystem getter, reports through
 * FFPMDetectorRegistry. No hook, no cvar write, no ini.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMLightweightCensusDetector final : public IFPMFix
{
public:
	static FFPMLightweightCensusDetector& Get();

	virtual const TCHAR* Name() const override { return TEXT("lightweight-census-detector"); }

	/** Any - actor tick/replication/component cost is identical in kind on a dedicated server
	 *  (no renderer needed to count actors or read a subsystem's map). */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Detect; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;

	/** `FPM.Detect.Lightweight` - census now against the given world. */
	static void RunNow(UWorld* World);

	/**
	 * A class needs at least this many live actor instances before its zero lightweight presence
	 * is reported. Chosen, not measured: below this, a unique/rare building is indistinguishable
	 * from a genuine trap case, and flagging it would be noise the reader learns to ignore - which
	 * is worse than not flagging it, per this project's own "a census that cries wolf gets
	 * ignored" discipline (see FPMMaterialEffectProbe's saturation cap for the same shape of
	 * choice). Ant can lower it via `FPM.Detect.LightweightThreshold` if the default hides
	 * something she wants to see.
	 */
	static int32 MinActorCountToFlag();

	/**
	 * ★ THE LIVENESS PROOF, WORLD-DEPENDENT. At `Arm()` (StartupModule, pre-world) neither
	 * `TActorIterator` nor the lightweight subsystem can report anything real, so `World == nullptr`
	 * is handled by printing that plainly rather than faking a check. Given a real `World` (the
	 * path `RunNow` always takes), the proof is against GUARANTEED vanilla content: any save with
	 * buildables placed has SOME class with a non-empty lightweight-instance entry (foundations,
	 * walls - lightweight since Satisfactory 1.0, per `FPMEnclosure.cpp:229`'s own note) and
	 * `TActorIterator<AFGBuildable>` finds at least one actor (the always-actor HUB). Both being
	 * true on a real world is the concrete non-trivial check; both being empty on a supposedly
	 * loaded save would mean one of the two read paths is broken, not that the save is empty.
	 *
	 * @return true if the checks available at this call passed (world-null calls only check that
	 *         neither API crashes on a null world, and say so).
	 */
	static bool SelfTest(UWorld* World);
};
