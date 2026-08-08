// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Module/RootInstance_FicsitsPerformanceManager.h"

#include "FicsitsPerformanceManager.h"

URootInstance_FicsitsPerformanceManager::URootInstance_FicsitsPerformanceManager()
{
	// This one flag is the whole of SML's discovery contract for a C++ module (ModModules.adoc:71-72).
	// Read the header before adding a second module of this type anywhere in the plugin.
	bRootModule = true;

	// Registration on a Mod Module is DECLARATIVE and belongs here in the constructor — the documented
	// pattern (ModModules.adoc:115-116). The instance-scoped arrays SML offers are ModConfigurations,
	// BlueprintHooks, GlobalItemTooltipProviders, WidgetBlueprintHooks, GameMaps, SessionSettings and
	// RemoteCallObjects (GameInstanceModule.h). FPM registers none of them yet, deliberately:
	//
	//   ModConfigurations - NOT USED. Settings ship as UFGUserSetting assets in the
	//   FicsitsPerformanceManager_Settings GameFeature so they land in the game's own options menu.
	//   SML's config system is Blueprint-first (its C++ CreateEditorWidget returns null), so a
	//   C++-only UModConfiguration registers and saves correctly while rendering an EMPTY page — the
	//   old mod proved that on a boot test.
}

void URootInstance_FicsitsPerformanceManager::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	// Super first: the base class is what performs the declarative registration above
	// (UGameInstanceModule::RegisterDefaultContent), and ModModule.h:62-63 requires the super call.
	Super::DispatchLifecycleEvent(Phase);

	// This log line is not decoration. It is the only proof available that SML discovered this module,
	// found exactly one root of this type, and reached each phase — and it is what the first boot test
	// checks, before anything depends on that being true.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] instance module: %s"), *LifecyclePhaseToString(Phase));
}
