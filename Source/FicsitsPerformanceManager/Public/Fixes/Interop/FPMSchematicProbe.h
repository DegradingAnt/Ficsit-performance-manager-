// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * SCHEMATIC ACCESS PROBE — LOG-ONLY. IT OVERRIDES NOTHING, REFUSES NOTHING, AND CHANGES NO ANSWER.
 *
 * Ant, 2026-08-08, on the old mod's forced-TRUE milestone override: *"maybe carry it with just
 * diagnostics and we'll see what happens? the guard you mentioned in nr 1 might be needed. check what
 * is needed here."*
 *
 * This is the "check what is needed" done first, and the answer it produced was **not a guard**.
 *
 * ★ WHAT THE CRASH DUMPS ACTUALLY SAY (31 dumps on disk, read 2026-08-08).
 * Six carry `CanGiveAccessToSchematic` in the CALLSTACK — not merely in the log, which every dump does
 * because the mod loads. All six are `EXCEPTION_ACCESS_VIOLATION reading address 0x00000000000002c0`,
 * and all six are on the GRANT path, not a query:
 *
 *     UFGSchematic::CanGiveAccessToSchematic()                        <- dies here
 *     FicsitPerformanceManager!HookInvokerExecutorGlobalFunction<...> <- our pass-through
 *     KPrivateCodeLib!HookInvokerExecutorGlobalFunction<...>          <- their hook
 *     AFGSchematicManager::Internal_CommitCurrentSchematicTransaction()
 *     AFGSchematicManager::GiveAccessToSchematics()
 *     AFGSchematicManager::GiveAccessToSchematic()
 *
 * Of the six: **four** have an FPM frame, **two do not**, and one of those two — `A981D1D4` — has
 * **neither FPM nor KPrivateCodeLib**. A crash that happens with no mod on the stack is a VANILLA
 * crash. FPM is a bystander in the four where it appears.
 *
 * ⚠ SO THE OLD OVERRIDE'S PREMISE IS DEAD, AND SO IS THE GUARD THAT WAS PAIRED WITH IT. The guard
 * tested `GetDefaultObject(false) != nullptr` on the theory that `0x2c0` is a read off a null CDO. That
 * guard SHIPPED, and the crashes continued. The old file's own comment says the ruling that followed:
 * *"DO NOT narrow this guard again without understanding the refusal. That is now the third time that
 * instruction has had to be written down in this file."* Carrying a fourth narrowing would be guessing
 * a fourth time. So this carries no guard at all — only the instrument that can end the guessing.
 *
 * ★ WHAT IT MEASURES, AND WHY EACH NUMBER EARNS ITS PLACE
 *  - every call, counted, so "armed and saw nothing" is distinguishable from "never armed",
 *  - **calls where the CDO IS NULL** — logged individually and unthrottled, because that is the old
 *    theory's predicted-fatal case. If a crash ever lands and this count is still zero, the null-CDO
 *    theory is dead by measurement rather than by argument,
 *  - calls with a null class, separately, because that is a different failure and was conflated before.
 *
 * ⚠ AND IT MUST NOT COST FRAMES — THE LAST ATTEMPT AT THIS FROZE THE GAME. Version 0.58.54 of the old
 * mod logged and flushed on EVERY call; the HUB and the build-menu search both enumerate every schematic
 * in the game, and Ant got *"the entire game freezes when opening the hub"*. So the hot path here is an
 * atomic increment and one pointer compare, with no string work and no I/O unless something anomalous
 * is actually found. The instrument must not cost more than the fault.
 *
 * ⚠ THE CRASH-TIME BREADCRUMB IS DELIBERATELY NOT REVIVED. The old mod printed the last-seen schematic
 * from `FCoreDelegates::OnHandleSystemError`; the 2026-08-08 triage measured it at **0 true positives
 * and 1 false positive** — that delegate does not fire for an access violation in a Shipping build, and
 * the one time it did print, it named a schematic on a stack that contained none.
 */
class FFPMSchematicProbe final : public IFPMFix
{
public:
	static FFPMSchematicProbe& Get();

	virtual const TCHAR* Name() const override { return TEXT("schematic-probe"); }

	/*
	 * Observation only, so there is no side to gate. Schematic grants are server-authoritative while the
	 * observed crashes are on Ant's client, and this writes nothing on either — no cvar, no config, no
	 * SaveGame field, no Override. `Any` is the contract's default posture and nothing here argues to
	 * leave it.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** UnknownCause: the family's mechanism is not receipted ON OUR STACK. The enum has four values and this is the honest
	 * one; the probe IS this family's origin-naming instrument. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::SchematicProbe; }

	virtual void Arm() override;
};
