// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ THE SAVESETTINGS INTERCEPTOR — the thing that stands between a TRANSIENT write and a PERMANENT
 * change to Ant's own settings file. Design P1.3 / §2.3.6.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ WHY IT IS MANDATORY, MEASURED 2026-08-09 RATHER THAN ARGUED.
 *
 * There was a live contradiction in this project's own records, and it decided whether this file needed
 * to exist at all:
 *   - `FPMCVarWriter.h:30-33` and design §2.3.6, from disassembly (AC4): `FGGameUserSettings` serialises
 *     every `mUserSettings` entry on every save with **NO DIRTY GATE**.
 *   - `FGUserSettingApplyType.h:101-102`, in the engine's own words: *"Returns a non empty FVariant if we
 *     have a value to actually save i.e the value is different from the default value and marked as
 *     dirty"* — which promises a gate.
 *
 * `FPM.D0` asked the running game, read-only, on 0.5.7. **28 cvar-backed settings would be written by a
 * save right now, and 16 of those 28 sit at exactly their default value** — `sg.TextureQuality` at `3`
 * with default `3` would still be persisted. The "different from the default" half of that comment is
 * false in practice. The disassembly reading is the correct one.
 *
 * So: any value FPM holds on a US_*-backed cvar at save time becomes the player's PERMANENT setting and
 * survives uninstall. That is the single failure the whole zero-residue posture exists to prevent, and
 * it is why clause 6 refuses the entire subset until this interceptor is proven.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ THE MECHANISM: STAND DOWN, LET THE GAME SERIALISE THE TRUTH, STAND BACK UP.
 *
 * Two hooks on `UFGGameUserSettings::SaveSettings` (`FGGameUserSettings.h:115`, virtual over the
 * engine's `ENGINE_API virtual void SaveSettings()`):
 *   - BEFORE: release every hold on a US_*-backed cvar. Release is the ENGINE's tagged-history `Unset`,
 *     so the cvar reverts to whatever the player's own layer said — we do not restore a remembered
 *     value, because a remembered value can be our own earlier write (the ratchet R33 killed).
 *   - AFTER: re-apply exactly those holds through the ordinary `Hold()` path.
 * The game therefore serialises the player's values, never ours, and the frames either side of the save
 * are the only ones where our value is absent.
 *
 * ⚠ IT USES THE PUBLIC WRITER API AND NOTHING ELSE. A second code path that can write cvars is a second
 * code path that can leak them, so this file never touches the ledger directly.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ THE THREE INVARIANTS §2.3.6 NAMES, AND WHY EACH IS NOT OPTIONAL
 *
 * 1. **ARM-TIME SELF-TEST.** It proves the hook actually installed, on FPM's own cvar, every boot —
 *    because what it is checking is an ENGINE behaviour a game update can change under us. A guard that
 *    silently stopped installing would look perfect right up until an uninstall left residue.
 *
 * 2. **PERMANENT FAIL-SAFE.** If the self-test fails, or a restore ever fails, this latches FAILED for
 *    the rest of the session and the writer refuses every mapped write from then on. It never re-arms
 *    optimistically. A guard that recovers by itself is a guard nobody investigates.
 *
 * 3. **REFUSE TO ARM WHILE HELD.** Arming while FPM already holds a mapped cvar would mean the first
 *    save is protected by a guard that never saw the write go up. Arming happens in `StartupModule`
 *    where nothing can be held yet, so this is cheap — and it is asserted rather than assumed, because
 *    "cannot happen" is how the 0x2c0 guard was justified three times.
 *
 * ★ CLAUSE 6 NOW RESTS ON THIS FILE — lifted 2026-08-09 by Ant's ruling, and here is exactly what that
 * means, because it is easy to read as a relaxation and it is not one.
 *
 * BEFORE: clause 6 refused every US_*-backed write outright, "until P1.3 ships the SaveSettings
 * interceptor". P1.3 shipped and nobody updated the gate, so the refusal outlived its own condition and
 * produced a deadlock — P1.5 Leg B must hold `t.MaxFPS` to discover whether the settings menu's APPLY
 * path (0x08 `SetByGameOverride`) outranks 0x07, clause 6 refused that hold, and lifting clause 6 was
 * gated on Leg B's answer. Ant: *"we'll have to lift it to get the awnser then. the law is more for
 * release than dev env."*
 *
 * AFTER: the writer permits a mapped write only while `IsHealthy()` is true. The guarantee moves from
 * "we never write these" to "we only write these while something is provably standing between the write
 * and the save file". Because `IsHealthy()` fails CLOSED in every uncertain state — before `Arm()`, after
 * any failure, mid-suspension — the unsafe cases are refused exactly as they were before.
 *
 * STILL UNANSWERED, and it is the point of Leg B: startup applies at `GameSetting` (0x02), measured on
 * 0.5.6, but the APPLY BUTTON is a different code path and may write at 0x08. If it outranks us,
 * §2.3.2's stated fallback engages. Both outcomes are a pass for the boot; only the HELD outcome licenses
 * the law write-back.
 */
class FFPMSaveSettingsInterceptor final : public IFPMFix
{
public:
	static FFPMSaveSettingsInterceptor& Get();

	virtual const TCHAR* Name() const override { return TEXT("save-settings-guard"); }

	/*
	 * `Any`. A dedicated server has a `UGameUserSettings` too and can serialise it, and the writer runs
	 * there. Gating this off the server would mean the one machine with no human watching is the one
	 * with no guard.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** Guard: the capture is the GAME's behaviour, not a bug of ours. We prevent the harm; we do not own
	 *  the cause, and we never will. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::SaveGuard; }

	virtual void Arm() override;

	/**
	 * ★ THE WRITER WILL ASK THIS BEFORE EVERY MAPPED WRITE — once clause 6 is lifted.
	 *
	 * ✔ WIRED 2026-08-09. `FPMCVarWriter`'s clause 6 now calls this before permitting any US_*-backed
	 * write, which is what this function was written for. It was marked NOT WIRED YET for a day, because
	 * a comment claiming a live integration that does not exist is the project's own named defect — and
	 * this note is kept rather than deleted so the next reader can see the claim was earned, not assumed.
	 *
	 * False when the hook did not install, when the self-test failed, or when a restore has ever failed.
	 * Fail CLOSED: it is also false before `Arm()` has run, so a write racing startup is refused rather
	 * than waved through. Not-knowing is not consent.
	 */
	static bool IsHealthy();

	/** Latch the permanent failure, with a reason that names the remedy. Public so the writer can trip it
	 *  too if it ever detects a leak from the other side. */
	static void Fail(const FString& Reason);

	/** How many saves were seen, and how many holds were stood down across them. `FPM.Diag.Dump` prints
	 *  these — a guard that has never fired should be visibly a guard that has never fired. */
	static void GetCounts(int32& OutSavesSeen, int32& OutHoldsSuspended);
};
