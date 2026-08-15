// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"

#include "FPMBlueprintContentSnapMode.generated.h"

/**
 * ★ THE MODE ITSELF IS NOTHING BUT A NAME — matching design doc S6, "MODE IDENTITY".
 *
 * `UFGHologramBuildModeDescriptor` adds no members over its parent (`FGHologramBuildModeDescriptor.h`
 * is a bare `UCLASS()` with only `GENERATED_BODY()`), and the parent's only piece of state is
 * `mDisplayName` (`FGBuildGunModeDescriptor.h` :26-27, `EditDefaultsOnly FText`, protected — settable
 * from any subclass's constructor, native or Blueprint). A NATIVE subclass with `mDisplayName` set in
 * its C++ constructor is materially identical to the `UBuildMode_Curve_C`-style Blueprint assets other
 * mods already ship (design doc S6's FModel receipt), so FPM ships no game asset for this feature.
 *
 * This class does not know it is a "content snap" mode. It carries a display string; Hook A
 * (`FPMBlueprintContentSnap.cpp`) is what makes `AFGBlueprintHologram` offer it, and Hook B is what
 * makes `IsCurrentBuildMode(StaticClass())` mean anything. Keeping the descriptor itself inert is
 * deliberate — a build-mode descriptor with logic in it would be a second place a bug could hide.
 *
 * ★ DISPLAY NAME IS A PLACEHOLDER, per plan step 1 / design S12 Q1 (Ant's call, LAW 17, not made
 * here). "Blueprint (Content Snap)" is the design's own first-listed proposal and the closest match
 * to vanilla's existing "Blueprint (Auto-Connect)" naming voice (Player_UI.csv,
 * "BuildModes/BlueprintSnapAutoConnect"). Unblocks the rest of the build without picking for her.
 */
UCLASS()
class FICSITSPERFORMANCEMANAGER_API UFPMBlueprintContentSnapBuildMode final : public UFGHologramBuildModeDescriptor
{
	GENERATED_BODY()

public:
	UFPMBlueprintContentSnapBuildMode();
};
