// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * BUILDABLE MESH PROBE — a diagnostic, not a fix. It reads and prints. It changes nothing.
 *
 * WHY IT EXISTS. The rain-occlusion repair derives an occlusion box from a class's static-mesh bounds,
 * and on 2026-08-08 the logs showed it running on a class and that class erroring ONE MILLISECOND
 * later:
 *     20:00:58.059  [FPM] rain occlusion fix (lightweight path): 1 class(es) repaired ...
 *                   (latest: Build_Concrete_Dome_Ceiling_8x8_C)
 *     20:00:58.060  LogRainSystem: Error: 0 Volume rain occlusion box found! ...
 *                   Build_Concrete_Dome_Ceiling_8x8_C Volume: IsValid=false
 * So the repair is not MISSING those classes, it is FAILING on them. The register's standing
 * explanation — reactive coverage, "at most 25 of 59" — is measuring the wrong thing.
 *
 * THE LEADING SUSPECT IS A FILTER, AND IT IS A HYPOTHESIS UNTIL THIS PROBE SAYS OTHERWISE. The old
 * collector accepted a component only when `SMC && SMC->GetStaticMesh() && !SMC->bHiddenInGame`.
 * Satisfactory renders buildables through UFGColoredInstanceMeshProxy, which does derive from
 * UStaticMeshComponent (FGColoredInstanceMeshProxy.h:15) — and an instanced proxy is plausibly hidden,
 * because the instance manager draws it instead. If so, every mesh gets skipped, the bounds come back
 * invalid, the repair takes its else-branch and NEVER WRITES A BOX, and vanilla logs the error anyway.
 *
 * ⚠ THAT IS A GUESS THAT FITS THE TIMING, WHICH IS EXACTLY THE KIND OF GUESS THIS PROJECT HAS SHIPPED
 * BEFORE. This probe replaces it with a reading: per component, what it actually is, whether it is
 * hidden, whether it has a mesh, what its bounds are, and what its collision says. Then the redesign
 * is aimed at the real cause instead of a plausible one.
 *
 * IT ALSO SIZES THE REPLACEMENT. Ant's proposal is to derive the box from COLLISION rather than from
 * render meshes — "cant we just make everything that has collision also have rain collision?" — which
 * is semantically the better source, since rain should be stopped by what is solid. The probe prints
 * both, so the two candidate sources can be compared per class before a line of the fix is written.
 *
 * EDITOR-SAFE BY CONSTRUCTION: it installs no hooks and mutates nothing, so unlike the fixes it is
 * useful in the editor, which is where a class can be inspected without a full game boot.
 */
class FICSITSPERFORMANCEMANAGER_API FPMMeshProbe
{
public:
	/**
	 * Registers the console commands. Called from StartupModule, in every build including the editor.
	 *
	 *   FPM.ProbeBuildable <name-or-path>   one class, every component, in full
	 *   FPM.ProbeBuildables                 every LOADED buildable class, one verdict line each
	 *
	 * The plural form is the one that answers the real question — how many classes would the mesh route
	 * fail on, and for which reason — because a single class can only ever confirm or refute a guess,
	 * while the sweep gives the distribution.
	 */
	static void RegisterConsoleCommands();

	/** Print one class in full. Safe on any UClass; a non-buildable is reported and skipped. */
	static void DumpClass(const UClass* BuildableClass);
};
