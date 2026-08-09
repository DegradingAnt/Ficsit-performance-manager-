// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Module/RootGameWorld_FicsitsPerformanceManager.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMUserSettingMap.h"
#include "Engine/World.h"

namespace
{
	/** ENetMode as a name. Hand-written rather than reflected so this cannot depend on the enum staying a UENUM. */
	const TCHAR* NetModeName(const ENetMode NetMode)
	{
		switch (NetMode)
		{
		case NM_Standalone:      return TEXT("standalone");
		case NM_DedicatedServer: return TEXT("dedicated server");
		case NM_ListenServer:    return TEXT("listen server");
		case NM_Client:          return TEXT("client");
		default:                 return TEXT("unknown");
		}
	}
}

URootGameWorld_FicsitsPerformanceManager::URootGameWorld_FicsitsPerformanceManager()
{
	// See URootInstance_FicsitsPerformanceManager's header before adding a second world module.
	bRootModule = true;

	// mChatCommands.Add(...) goes here once the diagnostics command exists. Registration is
	// declarative from the constructor and this array is the ONLY thing that makes a command
	// reachable — ChatCommands.adoc:46-47. The old mod shipped a correct, complete chat command that
	// was never added to any such array, so it could not be invoked, and its own header claimed it
	// worked. An implementation is not a feature until something registers it.
}

void URootGameWorld_FicsitsPerformanceManager::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	// Super first: the base performs the declarative registration (RegisterConstructionPhaseContent
	// and RegisterDefaultContent), and ModModule.h:62-63 requires the super call.
	Super::DispatchLifecycleEvent(Phase);

	// The net mode is logged because almost every FPM decision is sided. A fix that must run only on
	// the authority, and a readout that must run only on a client, both need this answer, and a world
	// module is the first place in the mod's life where it can be asked without guessing. Reading it
	// from the world beats inferring authority from a player controller.
	const UWorld* World = GetWorld();
	const TCHAR* Side = World ? NetModeName(World->GetNetMode()) : TEXT("no world");

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] game world module: %s (%s)"), *LifecyclePhaseToString(Phase), Side);

	/*
	 * THE LOADING-SCREEN SLOT. CONSTRUCTION is the first phase of a save load, so anything dispatched
	 * here runs while the loading screen is still up and a hitch costs the player nothing.
	 *
	 * This is deliberately a REGISTRY call, not a list of feature headers. The moment this module
	 * includes Fixes/..., every fix that wants load-time work becomes a dependency of the module — the
	 * coupling the old mod's structure died of.
	 */
	if (Phase == ELifecyclePhase::CONSTRUCTION)
	{
		/*
		 * ★ RE-READ THE USER-SETTING MAP FIRST, BEFORE ANY FIX RUNS.
		 *
		 * Ordering is the point: a fix's OnWorldLoad may hold a cvar, and clause 6's answer decides
		 * whether it may. Refreshing after the dispatch would judge this world's first writes against
		 * the previous world's picture.
		 *
		 * ⚠ AND IT MUST RE-READ, not read once. Mods register their settings as their game features
		 * activate, so a map captured at the earliest possible moment is a VANILLA map wearing a
		 * runtime label — and the settings FPM most needs to see are the mod-registered ones. Cheap:
		 * one map copy and a walk, on a frame where the loading screen is already up.
		 */
		FPMUserSettingMap::Refresh();

		FPMFixes::NotifyWorldLoad(GetWorld());
	}
}
