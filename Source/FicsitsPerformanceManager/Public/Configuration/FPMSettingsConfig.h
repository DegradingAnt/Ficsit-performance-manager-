// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Configuration/ModConfiguration.h"

#include "FPMSettingsConfig.generated.h"

/**
 * ★ FPM'S IN-GAME SETTINGS PAGE — through SML, which is what makes zero residue survivable.
 *
 * Ant, 2026-08-11, ruling on where settings live: *"also the mod settings need to be through sml"*, and
 * on how to build it: *"refine, dont copy"*.
 *
 * ═══ WHY SML AND NOT A `UFGUserSetting` ROW ═══
 *
 * A vanilla options row writes its `StrId` into `UFGGameUserSettings::mIntValues`, which is
 * `UPROPERTY(Config)` — so it lands in `GameUserSettings.ini` and **survives uninstalling the mod**.
 * The design doc spent a section weighing three ways to live with that. All three were answering the
 * wrong question. SML writes each mod's settings to its own file, verified from its source:
 *
 *     ConfigManager.cpp:309  GetConfigurationFolderPath() -> FPaths::ProjectDir() + "Configs/"
 *     ConfigManager.cpp:317  ... + FString::Printf("%s.cfg", *ConfigId.ModReference)
 *
 * `FactoryGame/Configs/FicsitsPerformanceManager.cfg`. One file, named after the mod, gone when the
 * player deletes it. *"It leaves nothing behind when you uninstall it"* stays literally true.
 *
 * ═══ ★ THE REFINEMENT OVER FPM1, WHICH IS THE POINT OF NOT COPYING IT ═══
 *
 * FPM1 kept config values in a PARALLEL STORE and shipped `FPMConfigBridge` to translate them into
 * runtime state. That made the same knob exist in three places — `FPMConfig.h`, the configuration
 * class, and the asset generator — and the project's own plan file records the consequence:
 * *"fewer than three is silent drift."*
 *
 * **Here there is no parallel store and no bridge.** FPM already has exactly one runtime mechanism for
 * everything a player can change: console variables, written through `FPMCVarWriter`. So this page is a
 * **VIEW ONTO THOSE CVARS**, not a second source of truth. A row's only job is to push its value into
 * its cvar; nothing reads config state at runtime, because the cvar IS the state.
 *
 * ⚠ AND THE MAPPING CANNOT DRIFT, BECAUSE THERE IS NO MAPPING TO MAINTAIN. Each row's subobject NAME is
 * its cvar name with the dots replaced: the row for `FPM.Upscaler.DLSSPreset` is named
 * `FPM_Upscaler_DLSSPreset`. The name IS the binding. Adding a lever is one line, and a typo produces a
 * loud "no such cvar" at startup rather than a row that silently controls nothing — which is precisely
 * the failure a hand-written mapping table invites.
 *
 * ═══ ⚠ WHAT IS DELIBERATELY *NOT* HERE ═══
 *
 * The ~31 `FPM.Fix.<Name>` toggles are not on this page. They are generated from the fix registry for
 * DEVELOPERS and for bisecting a bad boot — that is a debugging surface, and putting thirty of them in
 * a player's options menu would bury the handful of settings that are actually choices. They remain
 * available in the console, where the person who needs them already is.
 *
 * ═══ ⚠ WHY EVERY NODE IS CREATED AS A BLUEPRINT CLASS — SML's requirement, not a preference ═══
 *
 * `UConfigProperty::CreateEditorWidget` is a `BlueprintNativeEvent` whose C++ implementation returns
 * null. The real widgets live in SML's own Blueprint subclasses, and
 * `UConfigManager::CreateConfigurationWidget` just calls `RootSection->CreateEditorWidget`. A tree built
 * from the raw C++ classes registers, saves and loads perfectly while **rendering nothing at all** —
 * FPM1 hit exactly that and recorded it as boot-test bug 2. The config was never broken, only invisible.
 *
 * ⚠ `ConfigCategory` MUST STAY EMPTY for the same reason: the Mods menu looks a config up by mod
 * reference with a blank category, so a non-empty one renders an empty page while everything else keeps
 * working.
 */
UCLASS()
class FICSITSPERFORMANCEMANAGER_API UFPMSettingsConfig : public UModConfiguration
{
	GENERATED_BODY()

public:
	UFPMSettingsConfig();

	/**
	 * Pushes every row on the page into its matching cvar. Walks `BoundRows` unconditionally — every
	 * call re-writes and re-verifies ALL of them, not just whichever row prompted it.
	 *
	 * ★ CALL THIS DIRECTLY WHEN THE WRITE MUST HAVE HAPPENED BEFORE THE NEXT LINE RUNS (the
	 * POST_INITIALIZATION restore in RootInstance_FicsitsPerformanceManager.cpp is the one place that
	 * does). For anything reacting to a row CHANGING, bind to `RequestSync` instead — see its comment
	 * for why binding rows straight to this one was the bug.
	 *
	 * ⚠ IT VERIFIES. After writing, each value is read back and a mismatch is logged. A settings page
	 * that reports success while the cvar refused the write is the dead-instrument shape, and this one
	 * is easy to get wrong: several of the cvars it drives are owned by FactoryGame.
	 */
	UFUNCTION()
	void SyncAllToCVars();

	/**
	 * What every row's `OnPropertyValueChanged` is actually bound to — see `BindRow` in the .cpp.
	 *
	 * ★ COALESCES A BURST OF ROWS INTO ONE `SyncAllToCVars()`, NOT ONE EACH. `FOnPropertyValueChanged`
	 * carries no argument saying WHICH property moved, so per-row handlers would mean one `UFUNCTION`
	 * per setting and a hand-written binding for each — the drift surface this design exists to remove.
	 * One shared handler is still right; the bug was WHICH handler. Binding every row straight to
	 * `SyncAllToCVars` meant N rows moving together ran N full-table syncs back to back, one per row —
	 * found 2026-08-15 from six identical "settings applied: 6 row(s) written" lines inside one frame in
	 * Ant's log, i.e. 36 cvar writes and 36 read-back checks where one pass of 6 would do. `MarkDirty()`
	 * is the only thing in SML's ConfigProperty.cpp that broadcasts `OnPropertyValueChanged`
	 * (ConfigProperty.cpp:18-27), and `UConfigPropertySection::ResetToDefault` calls it once per CHILD as
	 * it cascades a reset down the tree (ConfigPropertySection.cpp:102-118) — six leaf rows under one
	 * root section is exactly six such calls. That is the confirmed mechanism; it does not depend on
	 * knowing which UI action asked for the reset, and the fix below does not depend on it either.
	 *
	 * This defers the actual sync to the next tick and lets `bSyncPending` absorb every additional
	 * firing that lands before it runs, so a burst of any size still costs exactly one
	 * `SyncAllToCVars()`. That single deferred pass is not reading stale data: by the time it runs, SML
	 * has finished setting every row in the burst, so it sees their final values, not a snapshot from
	 * partway through. A lone row changing (the ordinary case — someone moved one slider) still reaches
	 * `SyncAllToCVars()` on the very next tick, so a real edit is still applied and still reported.
	 */
	UFUNCTION()
	void RequestSync();

private:
	/** Every row created, so SyncAllToCVars can walk them without re-deriving the tree. */
	UPROPERTY()
	TArray<TObjectPtr<class UConfigProperty>> BoundRows;

	/** Set while a deferred `SyncAllToCVars()` is scheduled, so `RequestSync` can coalesce repeats. */
	bool bSyncPending = false;
};
