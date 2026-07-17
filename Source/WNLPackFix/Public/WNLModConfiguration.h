#pragma once

#include "CoreMinimal.h"
#include "Configuration/ModConfiguration.h"
#include "WNLModConfiguration.generated.h"

/**
 * The in-game config MENU for WNLPackFix (SML mod-settings page, like every other configurable mod).
 *
 * Defined entirely in C++ (the property tree is built in the constructor on the CDO) so the plugin
 * stays content-free — no cooked Blueprint asset needed. SML's config UI auto-generates the widgets
 * from this tree.
 *
 * DELIBERATE DESIGN: this menu exposes only the handful of knobs a player actually wants (on/off,
 * the two FPS targets, relief/bonus/cut switches, resolution floor). It uses its OWN ConfigId
 * category ("Menu" → saved to Configs/WNLPackFix/Menu.cfg), NOT the mod's main config file — SML
 * rewrites a registered config file to exactly its schema on load, which would wipe the ~50 advanced
 * keys in Configs/WNLPackFix.cfg. The governor overlays the menu values on top of the main config at
 * startup (menu wins for the keys it owns). Advanced tuning stays in the main JSON file.
 */
UCLASS()
class WNLPACKFIX_API UWNLModConfiguration : public UModConfiguration
{
	GENERATED_BODY()
public:
	UWNLModConfiguration();
};
