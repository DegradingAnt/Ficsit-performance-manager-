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
 * ⚠ WHAT THIS DOES **NOT** DO, STATED SO NOBODY READS MORE INTO IT.
 * It does not lift clause 6. Shipping the interceptor is the design's stated condition for lifting it,
 * but the lift is a separate, deliberate change that wants a boot behind it — and P1.5 Leg B (does the
 * menu's APPLY button write at 0x08 and outrank us?) is still unanswered. Startup applies at
 * `GameSetting` (0x02), measured on 0.5.6; the apply-button path is a different code path.
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
	 * ⚠ NOT WIRED YET, AND THE PRESENT TENSE WAS A LIE. This said "the writer asks this" while nothing
	 * called it (review 2026-08-09). Clause 6 currently refuses the whole US_*-backed set outright, so
	 * there is no mapped write for it to gate. It is here so the lift is one edit rather than a design
	 * task — but a comment claiming a live integration that does not exist is the project's own named
	 * defect, so it says what is true instead.
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
