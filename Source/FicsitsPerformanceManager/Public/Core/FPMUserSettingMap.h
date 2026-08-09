// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ WHICH CVARS THE GAME'S OWN SETTINGS SAVE WOULD CAPTURE. Design P1.3 / §2.3.6.
 *
 * One question, one answer, one declaration site: **"if FPM holds this cvar, will it end up in the
 * player's settings file?"** Clause 6 of the writer asks it before every write, the residue sentinel
 * asks it when classifying, and `FPM.D0` asks it when auditing. Three callers, one implementation —
 * a second copy is a bug by construction, and this project has already shipped the version of that bug
 * where a hand-maintained list drifted from what the game actually persists.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ THE MAPPING, FROM BYTES — AND IT IS NOT WHAT THE MAP'S KEYS SUGGEST.
 *
 * `UFGGameUserSettings::GetAllUserSettingsMap` (`FGGameUserSettings.h:330`) returns
 * `TMap<FString, TObjectPtr<UFGUserSettingApplyType>>`. It is tempting — and WRONG — to read those keys
 * as cvar names. They are setting IDs. The cvar name is the underlying setting's `StrId`, and ONLY when
 * that setting opts in:
 *
 *     FGUserSetting.h:183-189
 *       FString StrId;        // "The identifier in the system ... used to link this setting with a
 *                             //  underlying value in the game"
 *       bool UseCVar{false};  // "Should we manage and if needed create a cvar for this setting BASED
 *                             //  ON StrId."
 *
 * Confirmed on a real asset: `US_MaxFPS` carries `"StrId": "t.MaxFPS"`, `"UseCVar": true`. So `US_*` is
 * the ASSET-NAME convention and the StrId is the cvar — which is why P1.5 Leg B can call `t.MaxFPS` a
 * "US_*-backed" cvar without it being spelled `US_`.
 *
 * ⚠ THE DIFFERENCE IS NOT PEDANTIC: in vanilla, **188 of 254 settings have `UseCVar` false** and drive
 * no cvar at all. Treating every key as a cvar name reports those 188 as unprotected cvars, which is a
 * false alarm large enough to bury a real one.
 *
 * ⚠ AND THE RELATION IS MANY-TO-ONE. Several settings may name the same cvar — that is the documented
 * reason this map exists at all (`FGOptionInterfaceImpl.h:30-33`: *"the ID = OptionCVarName assumption
 * breaks ... we want to have a setting twice in different submenus"*). Keying our own set by StrId
 * collapses those correctly; keying by the map's key would not.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ WHY A RUNTIME READ AND NOT JUST THE SHIPPED TABLE.
 *
 * Because the table is VANILLA-ONLY and always will be — it is generated from an export of the base
 * game. Mods register their own settings, and the ones FPM most wants to reason about are exactly those:
 * the `LightSettings` mod's levers (`LS_SS_AttenuationRadius` and friends) are mod-side SessionSettings
 * assets that no export of FactoryGame can contain. Only the running process knows the full set.
 *
 * The table is therefore the FALLBACK, and when it is all we have, that fact is LOGGED. A fallback that
 * engages silently turns "we could not look" into "there is nothing there", which is the shape of
 * absence-claim this project has been burned by before.
 *
 * ⚠ ANSWERS ARE UNIONED, NEVER REPLACED. The runtime read is authoritative about what EXISTS, but it can
 * be performed too early (before mods register, before the settings object exists) and it can fail. A
 * union can only ever over-refuse, and over-refusing costs a log line while under-refusing costs a
 * permanent change to the player's own settings. Those costs are not symmetric, so the union stands.
 */
class FICSITSPERFORMANCEMANAGER_API FPMUserSettingMap
{
public:
	/** Where the current answer comes from. Reported by `FPM.Diag.Dump` and the writer's refusals. */
	enum class ESource : uint8
	{
		/** No runtime read has succeeded yet — answers come from the compiled tables alone. */
		TablesOnly,

		/** A runtime enumeration succeeded; its result is unioned with the tables. */
		RuntimePlusTables,
	};

	/**
	 * Bind the automatic read. Called once from StartupModule.
	 *
	 * ★ WHY THIS EXISTS RATHER THAN "TYPE FPM.D0 AFTER BOOT". Ant's standing rule: *"automate as much as
	 * possible by default. i dont like running around throwing commands around."* And the practical
	 * reason, measured 2026-08-09: console commands CANNOT be delivered to this game from the outside.
	 * UE strips `-ExecCmds` in Shipping, SML reimplements it (`SatisfactoryModLoader.cpp:218-227`), but
	 * Steam replaces the command line with its own launch options — both a direct launch and
	 * `steam://run/526870//<args>` came up with `-NO_EOS_OVERLAY -useallavailablecores` and nothing else.
	 * So a command that must be TYPED is a command that costs one of Ant's boots. This one reports itself.
	 *
	 * It hooks post-engine-init rather than running inline, because at StartupModule there is no engine
	 * and therefore no settings object — the read would fail every time and cache nothing.
	 */
	static void Init();

	/**
	 * THE QUESTION. True if holding `CVarName` risks it being serialised into the player's settings.
	 *
	 * ⚠ FALSE MEANS "NOT IN ANYTHING WE HAVE READ", NOT "PROVEN SAFE". While `Source()` is `TablesOnly`
	 * a mod-registered setting is invisible, so a caller acting on `false` before the runtime read has
	 * landed is acting on a vanilla-only picture. The writer states this in its refusal path.
	 */
	static bool IsBacked(const TCHAR* CVarName);

	/**
	 * Attempt the runtime enumeration. Safe to call at any time and as often as wanted — it no-ops
	 * cleanly when the settings object does not exist yet, which is most of startup.
	 *
	 * Returns true if a read succeeded THIS call. Re-reading is deliberate rather than cached-once: mods
	 * register settings as their game features activate, so a set captured at first opportunity can be
	 * incomplete and would then be wrong for the rest of the session.
	 */
	static bool Refresh();

	/** Which sources back the current answer. */
	static ESource Source();

	/** How many cvar-backed settings the last successful runtime read found. -1 if none has succeeded. */
	static int32 RuntimeCount();

	/** Print the source, the counts, and the fallback warning if it applies. */
	static void LogState(class FOutputDevice* Ar = nullptr);
};
