// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "RootInstance_FicsitsPerformanceManager.generated.h"

/**
 * FPM's ROOT GAME INSTANCE MODULE. Created once while the game launches, alive until the game closes,
 * survives world reloads, needs no world context (ModModules.adoc:103-106).
 *
 * THE NAME IS THE DOCUMENTED ONE. ModModules.adoc:99 asks for `RootInstance_<ModReference>`. SML does
 * not care — discovery is by the bRootModule flag and the docs say so at :59-60 — but the reason given
 * is identification in crash reports with many mods loaded, and reading other people's crash reports
 * is this mod's job. It keeps the convention it wants everyone else to keep.
 *
 * ONE ROOT PER TYPE PER PLUGIN, AND A SECOND ONE IS A HARD CRASH RATHER THAN A WARNING.
 * GameInstanceModuleManager.cpp:51-56 logs at Fatal: "Multiple root modules have been discovered ...
 * Only one root module is allowed". WorldModuleManager.cpp:78-83 is the identical check for world
 * modules. Nothing recovers from Fatal.
 *
 * AND THE TRAP THAT FOLLOWS FROM IT, WHICH THE DOCS DO NOT SPELL OUT: PluginModuleLoader.cpp:37 and
 * :41 union the loaded NATIVE subclasses with the BLUEPRINT assets tagged bRootModule=True, and both
 * sets resolve to an owning plugin the same way. A Blueprint child of this class inherits
 * bRootModule=true into its own asset tag, so it is discovered as a SECOND root for this same plugin.
 * The docs recommend making a Blueprint child the root (ModModules.adoc:68-72), because mod modules
 * help register asset-based content — that recommendation is an EITHER/OR, never an addition.
 *
 * The same trap applies to a C++ subclass: a submodule must derive from UGameInstanceModule directly,
 * or explicitly set bRootModule = false, or it becomes a second native root.
 *
 * WHY C++ IS THE RIGHT SIDE OF THAT EITHER/OR TODAY: nothing this module registers is an asset. FPM's
 * settings are UFGUserSetting assets delivered by the separate FicsitsPerformanceManager_Settings
 * GameFeature and found through the AssetManager scan path, not through this class. If that ever
 * changes, the migration is two edits and they must land in ONE commit: clear bRootModule here, and
 * set it on the Blueprint child. Doing only the second one crashes the game at launch.
 *
 * DO NOT REACH FOR A SUBSYSTEM FROM DispatchLifecycleEvent. GameInstanceModuleManager.cpp:68-70
 * dispatches all three phases from inside its own Initialize(), between :40 setting
 * bIsInitializingCurrently and :72 clearing it — so the subsystem collection is still initializing and
 * GetGameInstance()->GetSubsystem<T>() on one of ours is re-entrant. The old mod shipped exactly that
 * notify, it silently did nothing, and the governor ran a whole boot cycle on header defaults.
 * Ordering belongs to the consumer, which declares this manager as an InitializeDependency.
 */
UCLASS()
class FICSITSPERFORMANCEMANAGER_API URootInstance_FicsitsPerformanceManager : public UGameInstanceModule
{
	GENERATED_BODY()

public:
	URootInstance_FicsitsPerformanceManager();

	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;
};
