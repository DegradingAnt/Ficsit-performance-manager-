// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ARE FPM'S OWN SETTINGS ROWS REACHABLE? COUNT THEM, BEFORE ANY EXIST.
 *
 * Opens P4 (`_DESIGN-R2-2026-08-09.md:1566-1575`). The phase's own DONE condition is
 * *"row count logged = row count shipped"*, and this is that count.
 *
 * ★ BUILT BEFORE THE FIRST ROW EXISTS, ON PURPOSE.
 *
 * `UFGUserSetting` rows are `.uasset` content and can only be authored in the editor — Ant's work, not
 * mine. The failure mode this guards is the one `sf-scaffold` records as UNBOUNDED: if the asset
 * registry lacks the scan path at `StartupModule`, the scan yields nothing FOREVER, and the rescan path
 * is `#if WITH_EDITOR` only (`AssetManager.cpp:1046-1060`). In a cooked build there is no recovery and
 * no error — the options menu is simply empty.
 *
 * So the instrument ships first. The moment she adds one row, the next boot says whether the game can
 * see it. The alternative is authoring a dozen rows and finding out afterwards.
 *
 * ⚠ THE PLUMBING WAS VERIFIED FROM BYTES BEFORE THIS WAS WRITTEN, and it is already correct.
 * `Content/FicsitsPerformanceManager.uasset` (the `FGGameFeatureData`) carries
 * `PrimaryAssetTypesToScan` with `FGUserSetting` against `/FicsitsPerformanceManager/Settings`. Nothing
 * needs adding. `Content/Settings/` merely does not exist yet.
 *
 * ★ AND IT REPORTS TWO NUMBERS, WHICH IS THE WHOLE DESIGN.
 *
 * A single "0 rows" is the dead-instrument shape: it cannot distinguish *we shipped none* from *the
 * scan is broken*. So it prints FPM's count AND the game-wide `FGUserSetting` count:
 *
 *   ours 0, game-wide > 0   -> we shipped none. Expected today. The scan demonstrably works.
 *   ours 0, game-wide 0     -> NOT MEASURED. The asset manager is not enumerating this type at all,
 *                              and no conclusion may be drawn from our zero.
 *   ours N, game-wide > N   -> N rows are reachable. This is the pass.
 *
 * The game-wide count is the LIVENESS PROOF — vanilla ships many settings rows, so a game-wide zero is
 * a statement about the instrument rather than about FPM. Same shape as `FPMCVarWriter` proving its
 * release path on a probe cvar every boot instead of asserting it works.
 *
 * VIEWER ONLY. It installs no hook, writes no console variable, reads no ini, and changes nothing about
 * which settings exist or what they do.
 */
class FFPMSettingsAudit final : public IFPMFix
{
public:
	static FFPMSettingsAudit& Get();

	virtual const TCHAR* Name() const override { return TEXT("settings-audit"); }

	/**
	 * ⚠ NEVER ON A DEDICATED SERVER. `UFGUserSetting` rows drive the options MENU, which a dedicated
	 * server does not have. Every count there would read zero and the audit would report a catastrophe
	 * that is only the absence of a UI — the "instrument reports an absence as a finding" trap this
	 * project keeps paying for.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** A meter, not a repair. Nothing here claims a cause, and nothing here fixes one. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Settings; }

	virtual void Arm() override;

	/** Audits once the world is up, when the asset registry has had time to finish its scan. */
	virtual void OnWorldLoad(UWorld* World) override;

	virtual void Disarm() override;

	/** `FPM.Settings.Report` — count now and print. Safe to run at any time. */
	static void Report(FOutputDevice* Ar = nullptr);
};
