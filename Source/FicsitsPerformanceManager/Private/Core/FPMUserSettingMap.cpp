// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMUserSettingMap.h"

#include "FicsitsPerformanceManager.h"

#include "FGGameUserSettings.h"
#include "Settings/FGUserSetting.h"
#include "Settings/FGUserSettingApplyType.h"

#include "Engine/Engine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/OutputDevice.h"

// GENERATED, beside this file: FPMUserSettingTable::GDerivedUSBackedCVars, the 66 vanilla cvar-backed
// settings read from each asset's own StrId/UseCVar. Regenerate with
// 40-TOOLS/satisfactory/extract_user_settings.ps1.
#include "FPMUserSettingTable.g.h"

namespace
{
	/**
	 * THE LEGACY GUESSES — moved here from FPMCVarWriter.cpp so both tables and the runtime read live at
	 * ONE declaration site, which is the rule this facility exists to enforce.
	 *
	 * ⚠ RETAINED, NOT TRUSTED. Every name here was guessed from an ASSET name rather than read from the
	 * asset, and the guessing failed both ways: it missed 56 real cvar-backed settings (including
	 * `r.ContactShadows`, which design §2.3.6 records as having actually leaked on 2026-08-02), while
	 * refusing `r.Gamma` — a name the game does not use — instead of `r.TonemapperGamma`, which is what
	 * `US_Gamma` really drives.
	 *
	 * Of these 33, the derived table covers 10; the other 23 are either settings that drive no cvar or
	 * cvar names no setting claims. Both classes look safe to delete. They are kept anyway, because
	 * "looks safe on this evidence" is the exact sentence that preceded the 242-unmapped mistake, and a
	 * false refusal costs a log line where a false permission costs a permanent change to the player's
	 * own settings.
	 *
	 * ⚠ Name prefixed for the UNITY BUILD (FPMFixContract.h:166-171).
	 */
	const TCHAR* const GFPMUSLegacyGuesses[] =
	{
		TEXT("CSS.Conveyor.MaxDrawDistance"),
		TEXT("FG.AlwaysShowVehiclePaths"),      TEXT("FG.ArachnophobiaMode"),
		TEXT("FG.ConveyorItemFrequency"),       TEXT("FG.DisableNarrativeMessages"),
		TEXT("FG.DismantleCratePlacementMode"), TEXT("FG.DisplayHologramClearance"),
		TEXT("FG.HoldZipline"),                 TEXT("FG.HologramRotationMode"),
		TEXT("FG.MergeDismantleCrates"),        TEXT("FG.PauseGameInPauseMenu"),
		TEXT("FG.SampleCopyCustomization"),     TEXT("FG.SampleCopySettings"),
		TEXT("FG.SelectCancelSwap"),            TEXT("FG.VehiclePathRenderDistance"),
		TEXT("TSR.AntiAliasing"),               TEXT("r.Mobile.AntiAliasing"),
		TEXT("r.AntiAliasingMethod"),
		TEXT("r.Bloom.ScreenPercentage"),       TEXT("r.ScreenPercentage"),
		TEXT("r.TSR.History.ScreenPercentage"),
		TEXT("r.DefaultFeature.MotionBlur"),    TEXT("r.FastVRam.MotionBlur"),
		TEXT("r.Gamma"),
		TEXT("r.HairStrands.DeepShadow.Resolution"),
		TEXT("r.HeterogeneousVolumes.Shadows.Resolution"),
		TEXT("r.HeterogeneousVolumes.Tessellation.BottomLevelGrid.Resolution"),
		TEXT("r.HeterogeneousVolumes.CompositeWithTranslucency.Refraction.Tr"),
		TEXT("r.Mobile.ScreenSpaceReflections"),
		TEXT("r.ShadowQuality"),
		TEXT("r.VSync"),                        TEXT("r.Vsync"),
		TEXT("t.MaxFPS"),
	};

	/**
	 * Cvar names from the last successful runtime enumeration, stored LOWERCASED so membership is
	 * case-insensitive by construction rather than by every caller remembering to be.
	 *
	 * ⚠ Case matters here in practice, not in theory: `US_ScreenPercentage` declares its StrId as
	 * `r.screenpercentage` while the engine registers `r.ScreenPercentage`. A case-sensitive set would
	 * miss the guard on exactly the cvar most likely to be written.
	 *
	 * GAME THREAD ONLY — it is written from Refresh(), which touches UObjects.
	 */
	TSet<FString> GFPMUSRuntimeBacked;

	int32 GFPMUSRuntimeCount = -1;

	/** So the "running on the vanilla table only" warning is said once, not once per refused write. */
	bool bGFPMUSFallbackWarned = false;
}

void FPMUserSettingMap::Init()
{
	/*
	 * POST-ENGINE-INIT, not StartupModule. The module loads during engine init, so `GEngine` is not yet
	 * usable there and `GetGameUserSettings()` returns nothing — a read at arm time would fail on every
	 * boot and quietly leave us on the tables forever, which is indistinguishable in the log from
	 * "there were no settings".
	 *
	 * This makes a MAIN-MENU boot sufficient to answer P1.3's gate: no save load, no typing, no console.
	 * The world-load refresh in the game-world module still runs and still matters — it is the one that
	 * picks up MOD-registered settings once their game features have activated.
	 */
	FCoreDelegates::OnPostEngineInit.AddLambda([]()
	{
		Refresh();
		LogState();
	});
}

bool FPMUserSettingMap::Refresh()
{
	/*
	 * UObject access. Not a style guard — Refresh() is reachable from the writer, and the writer is
	 * reachable from fixes that run on Factory Tick workers. Enumerating UObjects there is the exact
	 * shape CSS warn about ("could at first look like it's working").
	 */
	if (!IsInGameThread()) { return false; }

	UFGGameUserSettings* Settings =
		Cast<UFGGameUserSettings>(GEngine ? GEngine->GetGameUserSettings() : nullptr);

	if (!Settings)
	{
		// Not an error. For most of startup there is no settings object yet, and the tables carry us.
		return false;
	}

	TMap<FString, TObjectPtr<UFGUserSettingApplyType>> All;
	Settings->GetAllUserSettingsMap(All);

	if (All.Num() == 0)
	{
		/*
		 * ⚠ REFUSE AN EMPTY ANSWER RATHER THAN CACHING IT. An empty map means "asked too early", not
		 * "this game has no settings" — and caching it would replace a correct vanilla table with a set
		 * that protects nothing, which is the worst available trade.
		 */
		return false;
	}

	TSet<FString> Fresh;
	Fresh.Reserve(All.Num());

	int32 NotCVarBacked = 0;
	int32 Unreadable = 0;

	for (const TPair<FString, TObjectPtr<UFGUserSettingApplyType>>& Pair : All)
	{
		const UFGUserSettingApplyType* Apply = Pair.Value;
		const UFGUserSetting* Setting = Apply ? Apply->GetUserSetting() : nullptr;

		if (!Setting) { ++Unreadable; continue; }

		/*
		 * ★ THE PREDICATE THAT MAKES THIS CORRECT, AND THE ONE A KEY-ONLY READ GETS WRONG.
		 *
		 * The map's KEY is a setting id, not a cvar name. A setting only owns a cvar when it says so,
		 * and then the cvar's name is its StrId (FGUserSetting.h:183-189). In vanilla 188 of 254
		 * settings answer false here — counting those as unprotected cvars would produce a false-alarm
		 * list long enough to hide a true one.
		 */
		if (!Setting->ShouldUseCVar()) { ++NotCVarBacked; continue; }

		/*
		 * StrId, NOT Pair.Key. Two settings may drive one cvar — the documented reason this map exists
		 * (FGOptionInterfaceImpl.h:30-33). Keying on StrId collapses that pair into one correct entry;
		 * keying on the map key would store two ids, neither of which is a cvar name.
		 */
		Fresh.Add(Setting->StrId.ToLower());
	}

	GFPMUSRuntimeBacked = MoveTemp(Fresh);
	GFPMUSRuntimeCount = GFPMUSRuntimeBacked.Num();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] user-setting map: %d of %d settings are cvar-backed (%d drive no cvar, %d unreadable). "
		     "This is the game's own answer and it includes mod-registered settings, which the compiled "
		     "table cannot."),
		GFPMUSRuntimeCount, All.Num(), NotCVarBacked, Unreadable);

	return true;
}

bool FPMUserSettingMap::IsBacked(const TCHAR* CVarName)
{
	if (!CVarName) { return false; }

	// Runtime first: it is the only source that knows about mod-registered settings.
	if (GFPMUSRuntimeBacked.Num() > 0 && GFPMUSRuntimeBacked.Contains(FString(CVarName).ToLower()))
	{
		return true;
	}

	for (const TCHAR* const Derived : FPMUserSettingTable::GDerivedUSBackedCVars)
	{
		if (FCString::Stricmp(CVarName, Derived) == 0) { return true; }
	}

	for (const TCHAR* const Guessed : GFPMUSLegacyGuesses)
	{
		if (FCString::Stricmp(CVarName, Guessed) == 0) { return true; }
	}

	/*
	 * ⚠ FALSE IS NOT A CLEAN BILL OF HEALTH. If the runtime read has not landed, this answer is drawn
	 * from two VANILLA-ONLY snapshots and cannot see a single mod-registered setting. Warned ONCE, at
	 * the first negative answer, because that is the moment the limitation starts mattering — and once
	 * rather than per-call because the writer's refusal path is not the place to generate volume.
	 */
	if (GFPMUSRuntimeCount < 0 && !bGFPMUSFallbackWarned)
	{
		bGFPMUSFallbackWarned = true;
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] user-setting map: answering from the COMPILED VANILLA TABLE only - the runtime "
			     "enumeration has not succeeded yet, so no mod-registered setting is visible. A 'not "
			     "backed' answer right now means 'not in the vanilla snapshot', NOT 'safe to write'."));
	}

	return false;
}

FPMUserSettingMap::ESource FPMUserSettingMap::Source()
{
	return GFPMUSRuntimeCount >= 0 ? ESource::RuntimePlusTables : ESource::TablesOnly;
}

int32 FPMUserSettingMap::RuntimeCount()
{
	return GFPMUSRuntimeCount;
}

void FPMUserSettingMap::LogState(FOutputDevice* Ar)
{
	auto Emit = [Ar](const FString& Line)
	{
		if (Ar) { Ar->Logf(TEXT("%s"), *Line); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *Line);
	};

	const int32 Derived = UE_ARRAY_COUNT(FPMUserSettingTable::GDerivedUSBackedCVars);
	const int32 Legacy = UE_ARRAY_COUNT(GFPMUSLegacyGuesses);

	Emit(FString::Printf(TEXT("user-setting map: %s"),
		Source() == ESource::RuntimePlusTables
			? TEXT("runtime enumeration + compiled tables")
			: TEXT("COMPILED TABLES ONLY - no runtime read has succeeded")));
	Emit(FString::Printf(TEXT("  runtime cvar-backed : %s"),
		GFPMUSRuntimeCount >= 0 ? *FString::FromInt(GFPMUSRuntimeCount) : TEXT("(not read)")));
	Emit(FString::Printf(TEXT("  derived table       : %d   (vanilla, generated from the assets)"), Derived));
	Emit(FString::Printf(TEXT("  legacy guesses      : %d   (retained so nothing narrows)"), Legacy));

	if (Source() == ESource::TablesOnly)
	{
		Emit(TEXT("  ** mod-registered settings are INVISIBLE in this state. Run FPM.D0 or load a world "
		          "and re-check before trusting a 'not backed' answer. **"));
	}
}
