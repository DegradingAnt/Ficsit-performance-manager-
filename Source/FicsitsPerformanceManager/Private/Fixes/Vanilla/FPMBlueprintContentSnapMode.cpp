// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Vanilla/FPMBlueprintContentSnapMode.h"

#define LOCTEXT_NAMESPACE "FPMBlueprintContentSnapMode"

UFPMBlueprintContentSnapBuildMode::UFPMBlueprintContentSnapBuildMode()
{
	// Placeholder pending Ant's ruling, design S12 Q1. See the header for why this specific string.
	mDisplayName = LOCTEXT("DisplayName", "Blueprint (Content Snap)");
}

#undef LOCTEXT_NAMESPACE
