#pragma once

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "WNLRootInstanceModule.generated.h"

/**
 * WNLPackFix's ROOT GAME INSTANCE MODULE — the SML-documented entry point for a mod's game-instance-
 * scoped registrations (docs.ficsit.app: Development/ModLoader/ModModules). SML discovers exactly one
 * root module per mod automatically via bRootModule=true; registrations happen DECLARATIVELY from the
 * constructor (the documented pattern), which replaced our earlier hand-rolled
 * FWorldDelegates+RegisterModConfiguration call.
 *
 * Scope note: the mod's native hooks and the perf governor live in the plain UE module
 * (FWNLPackFixModule::StartupModule) because they are engine-level and must arm before any world
 * exists. This class carries only what SML's module system owns: the in-game config menu.
 */
UCLASS()
class WNLPACKFIX_API UWNLRootInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()
public:
	UWNLRootInstanceModule();
};
