// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * SCHEMATIC NULL GUARD — P3.10(a), BUILT AGAINST THE CRASH CORPUS RATHER THAN AGAINST THE DESIGN LINE.
 *
 * ★ THE DESIGN'S SPEC FOR THIS FIX IS REFUTED BY OUR OWN BYTES, AND THAT IS WHY THIS FILE EXISTS IN A
 * DIFFERENT SHAPE THAN P3.10(a) ASKS FOR.
 *
 * The design (`_DESIGN-R2-2026-08-09.md`, P3.10(a)) specifies *"a SUBSCRIBE_METHOD entry hook on
 * `UFGSchematic::CanGiveAccessToSchematic` with a null/CDO check on the TSubclassOf argument"*, backed by
 * *"6/6 dump coverage"*. Both halves were re-derived from the dumps on 2026-08-10 and both are wrong:
 *
 *  - **THE COUNT IS 19, NOT 6.** Of 57 crash dumps on disk, nineteen carry
 *    `UFGSchematic::CanGiveAccessToSchematic` as **frame 0** of `<CallStack>` under
 *    `EXCEPTION_ACCESS_VIOLATION reading address 0x00000000000002c0`.
 *  - **THE ARGUMENT CHECK ALREADY SHIPPED AND ALREADY FAILED.** FPM1 0.58.52 installed exactly that
 *    guard — `Scope.Override(false)` when `!InClass || GetDefaultObject(false) == nullptr` — and
 *    **fourteen** of the nineteen dumps are on builds AFTER it: 0.58.53, .55, .61×2, .62×2, .66, .68,
 *    and .71×6. The stacks show the mechanism: FPM's pass-through frames sit BELOW vanilla's frame 0,
 *    so the guard evaluated the arguments, found them sound, and handed off. FPM1's own file records
 *    the conclusion — *"InClass was non-null, its CDO was non-null... Every argument was sound. The null
 *    lives INSIDE vanilla's body."*
 *
 * Re-implementing the specified check verbatim would therefore have shipped a fix already proven inert,
 * with a receipt claiming it covered a crash class it has never once caught.
 *
 * ★ WHAT THE NULL ACTUALLY IS — named from three independent witnesses.
 *
 *  1. `FGSchematic.h:158` documents the function: *"Checks for events and if
 *     mDependenciesBlocksSchematicAccess is true we check that all dependencies are met as well."*
 *  2. The events path runs through `AFGEventSubsystem` (`FGEventSubsystem.h:127-136`), whose accessor
 *     RETURNS NULL when the subsystem does not exist, and whose `IsEventActive` reads `mCurrentEvents`
 *     off `this` — a member read at a fixed offset off a null pointer, which is what `0x2c0` is.
 *  3. **TWELVE OF THE NINETEEN DUMPS RECORD `GameStateName = FGMainMenuState`** — the main-menu world,
 *     which has no event subsystem to fetch. The remaining seven are in-world (`BP_GameState_C`),
 *     consistent with the join transition where subsystems are not yet up: uptimes cluster at 32-103 s.
 *
 * That condition is decidable from OUR side of the hook, before vanilla runs, which the argument check
 * never was.
 *
 * ★ THE RIVAL EXPLANATION, AND WHAT KILLS IT. An earlier draft of this fix (parked 2026-08-09) argued
 * that `0x2c0` is `mDependenciesBlocksSchematicAccess` (`FGSchematic.h:267`) read off a **null CDO** —
 * the function is static and takes a `TSubclassOf`, so the only way it reaches an instance member is
 * through the default object. That reasoning is sound and the offset fits: `UFGSchematic` has ample
 * properties to put a member at 0x2c0, and so does an `AActor`-derived subsystem. **The offset alone
 * cannot tell the two apart.**
 *
 * What tells them apart is that the CDO theory has already been TESTED. FPM1 0.58.52 refused on exactly
 * `GetDefaultObject(false) == nullptr`, and fourteen crashes of this class followed it with FPM's
 * pass-through frames sitting in the stack — i.e. the CDO was non-null every time. A theory that
 * predicts a guard will catch the crash, tested against a guard that did not, is refuted. The event
 * subsystem is the surviving candidate, and it additionally explains the `FGMainMenuState` correlation,
 * which the CDO theory does not touch.
 *
 * ★ WHY IT REFUSES SO NARROWLY: BECAUSE THE HOUSE ALREADY RULED THAT BREAKING THE HUB IS WORSE.
 *
 * The obvious guard — refuse whenever the event subsystem is null — can refuse a grant vanilla would
 * have answered perfectly well, and that failure has a name here. Ant on FPM1 0.58.51: *"i cant input
 * stuff into the HUB for milestones for certain mods and even some vanilla ones."* FPM1's file states
 * the ruling that followed: *"the crash was survivable by rebooting, an unusable HUB is not."*
 *
 * So this refuses ONLY the combination that must dereference: **the schematic declares relevant events
 * AND the event subsystem is null.** A schematic with no relevant events never reaches the events code
 * on either plausible shape of vanilla's body (guard-then-fetch, or fetch-then-deref-in-loop), so it is
 * passed through untouched. In the case where it DOES refuse, vanilla's alternative was not a different
 * answer — it was the access violation.
 *
 * ★ THE BLAST RADIUS IS EIGHTEEN SCHEMATICS, AND EVERY ONE IS FICSMAS. MEASURED, NOT ARGUED.
 *
 * Ant asked the right question of this fix: *"that milestone guard needs to not break anything like it
 * did last time."* Counted from the game export on 2026-08-10 over the WHOLE tree
 * (`20-SOURCES/satisfactory/fmodel-exports`): **43 assets declare a non-empty `mRelevantEvents`, and
 * all 43 name the same event, `EV_Christmas`.** Of those, **18 are schematics** — the 14-asset
 * `Schematics/Research/XMas_RS` tree plus four `Events/Christmas/Calendar_Schematics/Ficsmas_Schematic_*`.
 * The other 25 are RECIPES, which this guard never sees. **Zero non-event schematics exist anywhere in
 * the export**, and the modded schematic trees present (CatwalkLadders, ContentLib) declare none.
 *
 * ⚠ THE FIRST VERSION OF THIS PARAGRAPH SAID FOURTEEN, AND IT WAS UNDER-SCOPED. It searched only
 * directories NAMED `Schematics` and so missed the four calendar schematics filed under
 * `Events/Christmas/`. Recorded rather than quietly corrected, because the failure is the recurring one
 * here: a count taken from a narrower scope than the claim it is used to support.
 *
 * The refusal condition requires a non-empty event list, so this guard is STRUCTURALLY INCAPABLE of
 * touching a HUB tier, a MAM research node, or any ordinary milestone — they declare no events, so the
 * branch cannot be reached for them however broken the world is. The 0.58.51 lockout
 * (*"i cant input stuff into the HUB for milestones"*) came from a guard that refused on a null world
 * context, which every caller can legitimately pass. This one cannot reproduce that shape.
 *
 * And for the fourteen it CAN refuse: a FICSMAS schematic is event-gated by vanilla anyway, and the
 * guard only fires when the event subsystem does not exist — which is precisely the state where vanilla
 * cannot answer the question either, and was about to crash trying.
 *
 * ⚠ AND THE EXCLUDED CASE IS COUNTED, SO THE REASONING ABOVE IS FALSIFIABLE. If a schematic has a null
 * event subsystem and NO relevant events, this guard passes it through and increments
 * `PassedEventlessWithNullSubsystem`. Should a `0x2c0` crash ever land while that counter is non-zero,
 * the narrowing is dead by measurement and the guard should widen to refuse on a null subsystem alone.
 * That is the same epistemics the sibling probe is built on, and it is the only honest way to ship a
 * conclusion drawn from a header comment rather than from vanilla's actual body.
 *
 * ⚠ ARM ORDER IS LOAD-BEARING: THIS MUST ARM **AFTER** `FFPMSchematicProbe`.
 * SML chains handlers in registration order and `TCallScope::Override` sets `bForwardCall = false`,
 * which stops the chain — every handler registered LATER is skipped
 * (`NativeHookManager.h:216-228`). Arming this first would silence the probe on exactly the calls
 * worth observing. The probe never overrides, so probe-then-guard preserves both.
 *
 * BEHAVIOUR SWITCH, NOT A DIAGNOSTIC ONE: `FPM.SchematicGuard 0` makes it observe without refusing, so
 * one boot can A/B whether the guard is what stopped a crash. `FPM.Diag.SchematicGuard` changes only
 * what it PRINTS. Both are our own cvars, written to no ini — zero residue is untouched.
 */
class FFPMSchematicNullGuard final : public IFPMFix
{
public:
	static FFPMSchematicNullGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("schematic-null-guard"); }

	/*
	 * Schematic grants are server-authoritative (`Internal_CommitCurrentSchematicTransaction` is frame 7
	 * of every one of these stacks), and the query runs on both sides. Nothing here touches a renderer,
	 * an audio device or input, so there is no subsystem-absence argument for leaving the default.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * Guard, exactly as the design rules — and deliberately NOT OriginNamed despite the three witnesses
	 * above. The cause is inferred from a header comment, a member offset and a game-state correlation;
	 * vanilla's body has not been read. A strong lead is not a receipt, and the enum's whole purpose is
	 * to stop that distinction eroding. The crash is also not ours to own: it arrives from another mod's
	 * grant path through vanilla code.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::SchematicGuard; }

	virtual void Arm() override;

	/** `FPM.SchematicGuard.Report` — the counters, without needing a crash to read them back. */
	static void LogStatus();

	/**
	 * Removes the hook.
	 *
	 * ⚠ Without this, `FPMFixes::DisarmAll()` reports this fix disarmed while its handler keeps
	 * running. Near-harmless at process exit, which is where DisarmAll was called from until P4.2
	 * shipped the master OFF switch (`FPM.Enabled 0`, `FPMMasterSwitch.cpp`) - that is why the
	 * omission survived that long. DisarmAll now also runs mid-session from that switch, which is
	 * exactly why this override has to be correct.
	 */
	virtual void Disarm() override;

private:
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle CanGiveAccessHandle;
};
