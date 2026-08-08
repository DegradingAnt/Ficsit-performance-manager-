// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * FPM's log category.
 *
 * THE NAME IS NOT A STYLE CHOICE. SML auto-creates a category called Log<PluginName> the first time a
 * Blueprint belonging to this plugin logs anything — BlueprintLoggingLibrary.cpp:46 builds the name as
 * `Log%s` from the plugin name. Naming ours identically puts C++ and Blueprint output in ONE category,
 * so a user who sets `LogFicsitsPerformanceManager=All` in Engine.ini gets all of it and not half.
 *
 * THE THIRD ARGUMENT MUST STAY `All`. Logging.adoc:65-78: if a category named Log<ModReference> has a
 * compile-time verbosity other than All, the engine re-creates it with All on the first Blueprint log
 * call, two verbosity levels end up declared for one name, and the game hard crashes. SML's own
 * creation path passes ELogVerbosity::All (BlueprintLoggingLibrary.cpp:19), so All is what we match.
 * The second argument is only the DEFAULT verbosity and does not have to match — Verbose does anyway.
 *
 * THE API MACRO IS LOAD-BEARING, NOT DECORATION. LogMacros.h:332-336 expands this to a plain
 * `extern struct ... CategoryName;` global, so without dllexport nothing outside this module can link
 * against it — the first UE_LOG from FicsitsPerformanceManagerEditor would be an LNK2019 that reads
 * like a broken build rather than a missing keyword. The engine puts the API macro in front of the
 * declaration for exactly this reason; see AIModule/Classes/BehaviorTree/BTNode.h:23 and seven
 * siblings.
 */
FICSITSPERFORMANCEMANAGER_API DECLARE_LOG_CATEGORY_EXTERN(LogFicsitsPerformanceManager, Verbose, All);

/**
 * The UE module. This is a compilation unit, and it is NOT the same thing as an SML Mod Module — those
 * are the UGameInstanceModule / UMenuWorldModule / UGameWorldModule lifecycle classes under Module/.
 * The two are orthogonal and conflating them is how a mod structure goes wrong.
 *
 * What belongs HERE is engine-level work that must run before any world exists: native hooks, and
 * later the governor. What belongs in the Mod Modules is anything SML scopes to a game instance or a
 * loaded world.
 */
class FFicsitsPerformanceManagerModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
