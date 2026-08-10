// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMSettingsAudit.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"

#include "Engine/AssetManager.h"
#include "Containers/Ticker.h"

namespace
{
	/**
	 * The primary asset type the GameFeatureData declares. Read from the asset's own bytes rather than
	 * assumed: `Content/FicsitsPerformanceManager.uasset` carries `PrimaryAssetTypesToScan` pairing
	 * `FGUserSetting` with `/FicsitsPerformanceManager/Settings`.
	 *
	 * ⚠ IF THIS STRING EVER DRIFTS FROM THE ASSET, THE AUDIT REPORTS ZERO FOREVER AND SOUNDS CONFIDENT.
	 * That is why the game-wide count exists beside it — a typo here zeroes BOTH numbers, which the
	 * report classifies as NOT MEASURED rather than as a clean result.
	 */
	const FPrimaryAssetType GFPMSettingAssetType(TEXT("FGUserSetting"));

	/** Our plugin's mount point. A row is "ours" when its object path starts with this. */
	const TCHAR* const GFPMSettingMount = TEXT("/FicsitsPerformanceManager/");

	FTSTicker::FDelegateHandle GFPMSettingsTicker;

	/**
	 * ⚠ WHY THIS WAITS INSTEAD OF COUNTING AT OnWorldLoad.
	 *
	 * The asset registry scan is asynchronous. Counting the instant the world comes up measures how far
	 * the scan happens to have got, not what shipped — the same single-instant error that produced the
	 * distance-field "RISING" misreading earlier today. Ten seconds is well past the scan on any machine
	 * that can run this game, and the count is stable from then on.
	 */
	constexpr float GFPMSettingsAuditDelaySec = 10.f;
}

FFPMSettingsAudit& FFPMSettingsAudit::Get()
{
	static FFPMSettingsAudit Instance;
	return Instance;
}

void FFPMSettingsAudit::Arm()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] settings audit ARMED - READ ONLY, no hook. It counts FPM's own UFGUserSetting rows "
		     "%.0f s after each world load, and prints the GAME-WIDE count beside them. Two numbers "
		     "because one cannot tell 'we shipped no rows' apart from 'the asset scan is broken', and "
		     "the second failure is silent and permanent in a cooked build. FPM.Settings.Report."),
		GFPMSettingsAuditDelaySec);
}

void FFPMSettingsAudit::OnWorldLoad(UWorld* World)
{
	if (GFPMSettingsTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GFPMSettingsTicker);
		GFPMSettingsTicker.Reset();
	}

	// Named first so the body never sits inside a macro — sf-scaffold section 7. No commas can be split
	// here even though this is not an SML macro, and keeping the habit costs nothing.
	auto OnElapsed = [](float /*Delta*/) -> bool
	{
		FFPMSettingsAudit::Report();
		return false; // one shot per world load
	};

	GFPMSettingsTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda(OnElapsed), GFPMSettingsAuditDelaySec);
}

void FFPMSettingsAudit::Disarm()
{
	/*
	 * Removed for the same reason the blueprint sweep gate unsubscribes: a ticker that survives Disarm
	 * fires into a torn-down module. It is also how the first distance-field sampler leaked a no-op
	 * delegate for a whole session, in the file whose subject was not spending frames.
	 */
	if (GFPMSettingsTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GFPMSettingsTicker);
		GFPMSettingsTicker.Reset();
	}
}

void FFPMSettingsAudit::Report(FOutputDevice* Ar)
{
	TArray<FString> Lines;

	UAssetManager* Manager = UAssetManager::GetIfInitialized();
	if (Manager == nullptr)
	{
		/*
		 * ★ NOT "ZERO ROWS". The asset manager not existing yet is a statement about WHEN this ran, and
		 * reporting it as a count would be a confident wrong answer.
		 */
		Lines.Add(TEXT("[FPM] settings audit: NO ASSET MANAGER YET - this is a statement about the "
		               "instrument, not about the rows. Nothing was counted. Run FPM.Settings.Report "
		               "again once you are in a world."));
	}
	else
	{
		TArray<FPrimaryAssetId> All;
		Manager->GetPrimaryAssetIdList(GFPMSettingAssetType, All);

		int32 Ours = 0;
		TArray<FString> OurNames;
		for (const FPrimaryAssetId& Id : All)
		{
			// GetPrimaryAssetPath resolves without loading the asset — the audit must not drag every
			// settings row in the game into memory just to count them.
			const FSoftObjectPath Path = Manager->GetPrimaryAssetPath(Id);
			if (Path.ToString().StartsWith(GFPMSettingMount))
			{
				++Ours;
				if (OurNames.Num() < 24) { OurNames.Add(Id.PrimaryAssetName.ToString()); }
			}
		}

		Lines.Add(FString::Printf(
			TEXT("[FPM] settings audit: %d FPM row(s) of %d FGUserSetting row(s) game-wide."),
			Ours, All.Num()));

		if (All.Num() == 0)
		{
			/*
			 * ⚠ THE NOT-MEASURED BRANCH, AND IT IS THE WHOLE REASON THE GAME-WIDE COUNT IS HERE.
			 * Vanilla ships many settings rows. Zero game-wide means the asset manager is not
			 * enumerating this primary asset type at all, so OUR zero carries no information either.
			 */
			Lines.Add(TEXT("[FPM]   ⚠ NOT MEASURED. Zero FGUserSetting rows GAME-WIDE, and vanilla ships "
			               "many - so the asset scan is not enumerating this type at all and our own "
			               "zero means nothing. Suspect the PrimaryAssetTypesToScan entry, not FPM's "
			               "row count."));
		}
		else if (Ours == 0)
		{
			Lines.Add(TEXT("[FPM]   0 FPM rows, and that is EXPECTED until Content/Settings/ has assets "
			               "in it. The scan itself is proven working by the game-wide count above. The "
			               "GameFeatureData already declares FGUserSetting against "
			               "/FicsitsPerformanceManager/Settings - verified from the .uasset bytes - so "
			               "a row authored there will be picked up with no further plumbing."));
		}
		else
		{
			Lines.Add(TEXT("[FPM]   FPM rows are REACHABLE. These are the ones the options menu can see:"));
			for (const FString& N : OurNames)
			{
				Lines.Add(FString::Printf(TEXT("[FPM]     %s"), *N));
			}
			if (Ours > OurNames.Num())
			{
				Lines.Add(FString::Printf(TEXT("[FPM]     ... and %d more (listing capped at %d)"),
					Ours - OurNames.Num(), OurNames.Num()));
			}
		}
	}

	for (const FString& L : Lines)
	{
		if (Ar != nullptr) { Ar->Log(L); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *L);
	}
}

static FAutoConsoleCommandWithOutputDevice GFPMSettingsReportCmd(
	TEXT("FPM.Settings.Report"),
	TEXT("Count FPM's own UFGUserSetting rows, and the game-wide FGUserSetting total beside them, so a "
	     "zero can be told apart from a broken asset scan."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMSettingsAudit::Report(&Ar);
	}));
