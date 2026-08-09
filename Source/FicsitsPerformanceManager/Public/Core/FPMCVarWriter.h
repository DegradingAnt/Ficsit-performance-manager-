// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

/**
 * ★ THE ONE PATH BY WHICH FPM IS ALLOWED TO WRITE A CONSOLE VARIABLE. Design §2.3, built at P1.2.
 *
 * Not a convenience wrapper. It exists because the old mod wrote cvars from wherever it happened to be
 * standing, and every catalogued corruption in this project's history came from one of two things —
 * PERSISTENCE or PRIORITY — never from cvars as such. A single choke point is the only place those two
 * can actually be enforced, and code that bypasses it is a review finding by construction.
 *
 * ⚠ THIS IS THE P1.2 SCOPE: clauses 1-5, 7, 8. **Clause 6 REFUSES the US_*-backed set entirely** until
 * P1.3 ships the SaveSettings interceptor. Refusing is not a stub — it is the correct behaviour for a
 * writer that cannot yet guarantee the value will not be serialised into the player's own settings.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * WHAT IT REFUSES, AND WHY EACH REFUSAL EARNED ITS PLACE
 *
 * 1. **A cvar the console manager cannot find.** Kills the dead-cvar class: `FG.ConveyorItemFrequency`
 *    and `r.NGX.DLSS.Quality` were written for months and every measurement attributed to them was
 *    attributed wrongly. A silent no-op is the worst possible outcome, so this one logs loudly.
 *
 * 2. **Any `sg.*` write.** Scalability groups leak through `Scalability::SaveState` with NO gate. The
 *    ladder expands a group into its member cvars and drives the members; it never writes the group.
 *
 * 3. **The US_*-backed set** (clause 6, until P1.3). `FGGameUserSettings` serialises every
 *    `mUserSettings` entry on every save with **no dirty gate**, so any value FPM is holding on a
 *    US_*-backed cvar at save time becomes the player's PERMANENT setting — residue that survives
 *    uninstall. That is the one failure this mod's whole residue posture exists to prevent.
 *    ⚠ AND THE LIST IS KNOWN-INCOMPLETE, WHICH IS THE POINT: of 272 US_* assets, **242 are UNMAPPED** —
 *    no candidate cvar name at all. Absence from the map is an ABSENCE CLAIM, not safety. That is why
 *    clause 6 refuses the whole set rather than filtering it, and why P1.3 reads the map at RUNTIME
 *    (`UFGGameUserSettings::GetAllUserSettingsMap()`) instead of trusting this shipped table.
 *
 * 4. `ECVF_SetByConsole`, `ForceSetValue`, `ForceSetPendingAppliedValue`. The last is the worst of the
 *    three: it bypasses the dirty gate on restart-required settings. None of them is reachable through
 *    this API, which is the only way to make "do not use it" true rather than aspirational.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ PRIORITY, AND THE OPEN QUESTION ABOUT IT
 *
 * Writes go at **`ECVF_SetByPluginHighPriority` (0x07)**, which sits above scalability (0x01),
 * game settings (0x02) and device profiles (0x06), and below commandline (0x0C), SetByCode (0x0D) and
 * console (0x0E) — so Ant's own console write still beats us, deliberately.
 * (`IConsoleManager.h:143-171`, read 2026-08-09.)
 *
 * ⚠ **`ECVF_SetByGameOverride` (0x08) SITS ABOVE US AND IS DOCUMENTED AS THE SLOT FOR GameUserSettings
 * FIELDS** (`IConsoleManager.h:158-159`). If FactoryGame's settings-apply path writes US_*-backed cvars
 * at 0x08, the game's own re-apply beats our hold SILENTLY — a lower-priority Set is simply ignored.
 * Nothing in source answers it. **P1.5's proof boot is where it gets answered**, and until then no
 * document may claim "0x07 wins" about the US_* subset. This is also a second, independent reason
 * clause 6 refuses that subset today.
 *
 * ★ RELEASE USES THE ENGINE'S OWN MECHANISM, NOT A LOWER WRITE. 0x05/0x07 are ARRAY-typed priorities:
 * the engine's own comment calls them *"used with the History concept to restore cvars on plugin
 * unload"* (`IConsoleManager.h:152-158`). So releasing is `Unset(priority, Tag)` (`:570`), which REMOVES
 * our entry from that history. Writing a lower value instead would APPEND to the array and leave our
 * value in the stack forever — a leak that looks exactly like a working revert.
 * `UnsetAllConsoleVariablesWithTag` (`:1243`) is the engine-native one-call ReleaseAll the OFF switch
 * rides. **The ledger is the AUDIT of what we hold; the engine's history is the MECHANISM that lets go.**
 * Nothing is captured and restored, so a release can never corrupt anything.
 */
enum class EFPMLease : uint8
{
	/** Released at ShutdownModule. The default, and correct for anything Core-owned. */
	Module,

	/**
	 * Released when the owning world-scoped object ends play.
	 *
	 * ⚠ THIS EXISTS BECAUSE THE WRITER OUTLIVES ITS CONSUMERS. The writer is module-lifetime; the
	 * governor and sensors are world-lifetime. Without a world lease, quit-to-menu leaves the MENU world
	 * holding gameplay values, and the next world's governor cannot re-register — "the governor is dead
	 * from the second world". Owner identity is a stable NAME, so the next world's instance RECLAIMS
	 * rather than colliding.
	 */
	World,
};

class FICSITSPERFORMANCEMANAGER_API FPMCVarWriter
{
public:
	static FPMCVarWriter& Get();

	/** Every write we make carries this tag, and it is what the engine-native release keys on. */
	static const FName& Tag();

	/**
	 * Hold `CVarName` at `Value` on behalf of `Owner` until released.
	 *
	 * Returns false and LOGS THE REASON on every refusal — a silent failure here is the exact shape that
	 * made months of measurements wrong. `Reason` is free text and appears in the ledger; write it for
	 * the person reading a support dump, not for yourself.
	 */
	bool Hold(FName Owner, const TCHAR* CVarName, const TCHAR* Value, const TCHAR* Reason,
	          EFPMLease Lease = EFPMLease::Module);

	/** Release one hold. Safe to call when we do not hold it — says so rather than pretending. */
	bool Release(FName Owner, const TCHAR* CVarName);

	/** Release every hold belonging to one owner. The world-lease teardown path. */
	int32 ReleaseOwner(FName Owner);

	/** THE OFF SWITCH. Engine-native, one call, then the ledger is emptied. */
	void ReleaseAll(const TCHAR* Reason);

	/** Print every live hold with its prior value and prior SetBy. Bound to `FPM.Changes`. */
	void LogLedger() const;

	/**
	 * ★ THE SELF-TEST, RUN AT ARM, ON FPM'S OWN CVAR — never on a game cvar.
	 *
	 * write -> read back -> release -> read back, verifying the value AND the SetBy return to what they
	 * were. This is P1.2's stated VERIFY, and it runs every boot rather than once in a test branch,
	 * because the thing it is checking is an ENGINE behaviour that a game update can change under us.
	 * A writer whose release path silently stopped working would otherwise look perfect right up until
	 * an uninstall left residue behind.
	 */
	bool SelfTest();

	/** True while a hold exists for this cvar, whoever owns it. */
	bool IsHeld(const TCHAR* CVarName) const;

private:
	struct FHold
	{
		FString CVar;
		FString Value;
		FString PriorValue;
		EConsoleVariableFlags PriorSetBy = ECVF_Default;
		FName Owner;
		FString Reason;
		EFPMLease Lease = EFPMLease::Module;
	};

	/** Refusal checks, in the order they are cheapest. Returns nullptr and logs if the write is refused. */
	IConsoleVariable* Vet(FName Owner, const TCHAR* CVarName) const;

	TArray<FHold> Ledger;
};
