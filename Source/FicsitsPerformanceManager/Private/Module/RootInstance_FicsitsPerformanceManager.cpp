// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Module/RootInstance_FicsitsPerformanceManager.h"

#include "FicsitsPerformanceManager.h"
#include "Configuration/FPMSettingsConfig.h"

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
	//   ModConfigurations - NOW USED, and the note that used to sit here was half right for the wrong
	//   reason. It said settings would ship as UFGUserSetting assets "so they land in the game's own
	//   options menu", and warned that SML's config system is Blueprint-first.
	//
	//   The Blueprint-first part is TRUE and is handled: UFPMSettingsConfig creates every node as
	//   SML's BP_ConfigProperty* subclass, which is exactly what the old mod's boot test taught.
	//
	//   The rest was overturned by Ant on 2026-08-11 - "also the mod settings need to be through sml" -
	//   and by what a vanilla row actually costs: its StrId lands in UFGGameUserSettings::mIntValues,
	//   which is UPROPERTY(Config), so it persists into GameUserSettings.ini and SURVIVES UNINSTALL.
	//   SML writes to Configs/FicsitsPerformanceManager.cfg instead (ConfigManager.cpp:309-317) - one
	//   file, named after the mod, gone when the mod goes. Zero residue stays literally true.
	ModConfigurations.Add(UFPMSettingsConfig::StaticClass());
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
