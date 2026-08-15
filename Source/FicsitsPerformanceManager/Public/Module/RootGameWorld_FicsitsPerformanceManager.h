// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "RootGameWorld_FicsitsPerformanceManager.generated.h"

/**
 * FPM's ROOT GAME WORLD MODULE. Created once per save load — including autosaves — and destroyed when
 * that save is exited (ModModules.adoc:143-149). It is a separate class from the instance module
 * because the two have different lifetimes and SML discovers them separately; one root of EACH type is
 * allowed, so the two coexist rather than compete.
 *
 * The duplicate-root trap, the Blueprint-child either/or and the do-not-touch-subsystems rule are all
 * documented on URootInstance_FicsitsPerformanceManager. They apply here identically — the Fatal for
 * world modules is WorldModuleManager.cpp:78-83.
 *
 * WHAT THIS MODULE IS FOR. SML scopes four things to a loaded world (GameWorldModule.h): schematics,
 * MAM research trees, chat commands, and resource-sink point tables. FPM will use exactly one of them,
 * mChatCommands, for the in-game diagnostics command. It is also the correct home for anything that
 * must run per save load rather than per session — which is what the dead-player-state repair is.
 *
 * KEEP IT THIN. Engine-level work that must arm before any world exists belongs in
 * FFicsitsPerformanceManagerModule::StartupModule, not here.
 *
 * SLICE 4, §5.9. This module registers `AFPMHostProbeSubsystem` into `ModSubsystems` from the
 * constructor below, the same declarative shape `mChatCommands` already documents.
 * `UGameWorldModule::DispatchLifecycleEvent` walks `ModSubsystems` during
 * `RegisterConstructionPhaseContent`, called from `Super::` BEFORE `FPMFixes::NotifyWorldLoad` runs
 * (this file's `.cpp`), so by the time any fix's `OnWorldLoad` sees a world, the probe has already
 * spawned there if this machine is authoritative. See `FFPMHostTier` (`Session/FPMHostTier.h`) for what
 * turns that into the FULL / NO-HOST-REPLICA tier.
 */
UCLASS()
class FICSITSPERFORMANCEMANAGER_API URootGameWorld_FicsitsPerformanceManager : public UGameWorldModule
{
	GENERATED_BODY()

public:
	URootGameWorld_FicsitsPerformanceManager();

	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;
};
