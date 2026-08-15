// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMLeverTypes.h"

/**
 * ★ SLICE 2 -- THE LEVER REGISTRY. Design §9.1 (M-LEVER) / §14 Slice 2's "writer-backed lever
 * registry with capability probes and the alias table". FPMLeverTypes.h is the schema this file
 * enforces; read that file's header first.
 *
 * ⚠ WHAT THIS BUILDS. The data model and its enforcement: registration (with Law 1 and clause-2
 * refusal built structurally in, not left to a comment), the alias table (ScalabilityGroup lever
 * -> member cvars, from the live BaseScalability.ini), capability probing (§3.12), the anti-
 * ratchet baseline-capture primitive (Law 3), and an honestly-split read side (what FPM itself
 * asked for vs what the cvar currently reports -- never a claim about hardware truth).
 *
 * ⚠ WHAT THIS DOES NOT BUILD, on purpose. The stage tables (the real B1-B6/K1-K4 lever CONTENT --
 * a separate §14 Slice-2 bullet), the give/take walk, the steering gate terms ("at floor",
 * "GPU-bound", "bench-worthwhile" -- §3.2), mode selection, the bench, and any UI. This ships with
 * self-test fixture levers only, registered from its own Arm() -- see FPMLeverRegistry.cpp's
 * `RegisterSelfTestLevers`. A future stage-table file registers real levers through the SAME
 * RegisterWritable/RegisterReadOnly API, called from that file's own Arm(), which must run before
 * this registry's OnWorldLoad (Arm order across fixes does not matter here -- probing happens at
 * world load, not at Arm, for the same reason FPMServerLevers defers its own audit that far: a
 * cvar owned by a module that has not finished loading does not exist yet at StartupModule).
 *
 * ★ THE READ SIDE, STATED HONESTLY (measured this session: FPM read DLSS Preset C from the cvar
 * while the DLL was actually running Preset K, driver-overridden, one line apart in the same log).
 * Two methods, two different claims, and a third claim this class refuses to make:
 *   - `GetOurHold`            -- what FPM itself asked for (FPMCVarWriter's own ledger). Always
 *                                 accurate to what we wrote, because it IS what we wrote.
 *   - `GetCVarRequestedValue` -- what the console variable currently reports, from ANY writer
 *                                 (us, the user's console, another mod). Knowable, but not proof
 *                                 anyone acted on it.
 *   - NO `GetObservedValue`. What the GPU/driver/DLL is actually doing is not reachable through
 *     the console-variable system at all. A caller that needs that has to build a dedicated
 *     detector (M-DETECT, Slice 4) reading the subsystem's own state -- this class will not
 *     pretend to answer a question it has no channel for.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMLeverRegistry final : public IFPMFix
{
public:
	static FFPMLeverRegistry& Get();

	virtual const TCHAR* Name() const override { return TEXT("lever-registry"); }

	/** Any -- the CONTAINER is side-agnostic (it merely stores and probes definitions); each
	 *  registered lever states its own EFPMFixSide (GPU-side levers default NeverOnDedicatedServer,
	 *  FPMLeverTypes.h). */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** UnknownCause -- the same choice and the same reason FPMServerLevers.h:63-68 gives: nothing
	 *  here is being FIXED, the entire value is structural enforcement and a fact the design does
	 *  not have yet (which levers exist and are writable on THIS build). */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::LeverRegistry; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;
	virtual void Disarm() override;

	/**
	 * Register a lever whose value FPM may WRITE (through FPMCVarWriter -- this class never calls
	 * `->Set()` itself; applying a value is the governor's job, a later slice).
	 *
	 * ★ LAW 1, STRUCTURALLY. Refuses (returns nullptr, logs Error, the definition never enters the
	 * live registry -- `Levers.Find()` can never return it) any definition naming a US_*-backed
	 * cvar (FPMUserSettingMap::IsBacked), or a direct `sg.*` write (FPMCVarWriter clause 2), or a
	 * baseline-comparing policy with BaselineSource left at NotApplicable (Law 3). This is the
	 * suspenders to FPMCVarWriter's own belt: a lever that fails here never reaches a live Hold().
	 *
	 * @return the stored definition, or nullptr if refused. The refusal reason is logged and kept
	 *         in the registry's refusal ledger for `ReportNow`, but the definition itself is not.
	 */
	const FFPMLeverDefinition* RegisterWritable(FFPMLeverDefinition Definition);

	/**
	 * Register a lever that the registry may READ but will never hold a write for. No Law-1 / clause-2
	 * restriction applies -- nothing is ever written for a read-only entry, so there is no residue
	 * hazard to structurally refuse.
	 */
	const FFPMLeverDefinition* RegisterReadOnly(FFPMLeverDefinition Definition);

	/** One registered lever, or nullptr (unknown name, or the name was REFUSED at registration and
	 *  therefore never lived here -- see EFPMLeverAvailability::Refused's own comment). */
	const FFPMLeverDefinition* Find(FName LeverName) const;

	/** Every registered lever (writable and read-only). Read-only view for a future consumer
	 *  (the governor, a diagnostic) -- nothing outside this class mutates the map. */
	const TMap<FName, FFPMLeverDefinition>& GetAll() const { return Levers; }

	/**
	 * Rebuild the alias table from the live BaseScalability.ini (`GConfig` + `GScalabilityIni`,
	 * never a raw file re-parse) for every currently-registered ScalabilityGroup-backed lever's
	 * GroupName, across tiers 0-3. Tier "Cine" is deliberately excluded -- no lever in any cited
	 * table reaches it, and including an unused tier is how a table grows entries nobody checks.
	 *
	 * Safe to call more than once; a later stage-table registration that adds a new group is picked
	 * up by the next call, not retroactively.
	 */
	void RefreshAliasTable();

	/** The member cvar names a ScalabilityGroup lever's group contains AT ONE TIER, or nullptr if
	 *  the group or tier is not in the table (RefreshAliasTable has not run, the group has no
	 *  registered lever, or the tier does not exist in BaseScalability.ini). Which tier a caller
	 *  should ask about is a steering decision -- this call answers only "what does tier N contain",
	 *  never "what tier are we at now". */
	const TArray<FString>* GetAliasMembers(const FString& GroupName, int32 Tier) const;

	/**
	 * ★ THE VALUE BaseScalability.ini PRESCRIBES FOR ONE MEMBER AT ONE TIER.
	 *
	 * A group step is not a write to the group -- FPMCVarWriter clause 2 refuses every `sg.*` write --
	 * so the only way to MOVE a scalability group is to write the values its target tier prescribes to
	 * the members themselves. The names alone cannot do that, which is why this exists.
	 *
	 * ⚠ NAMES AND VALUES ARE FILLED IN ONE LOOP, from one walk of one config section, so they cannot
	 * drift apart. That is deliberate: two structures built separately from the same source is exactly
	 * the shape that made a fix in one branch not a fix.
	 *
	 * @return false when the group, the tier or the member is not in the table. OutValue is untouched.
	 */
	bool GetAliasMemberValue(const FString& GroupName, int32 Tier, const FString& Member,
	                         FString& OutValue) const;

	/** What FPM itself currently asks for -- FPMCVarWriter's own ledger, not a cvar read. False
	 *  means "we are not holding this lever's cvar", not "nobody is". See the class doc comment. */
	bool GetOurHold(FName LeverName, FString& OutValue) const;

	/** What the console variable currently reports, from whichever writer holds it. See the class
	 *  doc comment -- this is NOT a claim about hardware/driver truth. */
	bool GetCVarRequestedValue(FName LeverName, FString& OutValue) const;

	/**
	 * ★ LAW 3'S ENFORCEMENT POINT. Captures `LeverName`'s baseline exactly once, from the live cvar,
	 * and only when it is safe: the lever must declare BaselineSource=CapturedOnce (RegisterWritable
	 * already refused any baseline-comparing policy that did not), and FPMCVarWriter must NOT
	 * currently hold that cvar -- reading it while we hold it risks reading our own prior write back
	 * as the "baseline", which is the exact ratchet failure this law exists to prevent.
	 *
	 * Idempotent: a second call on an already-captured lever returns true without re-reading. That is
	 * the "once" guarantee made observable rather than merely claimed.
	 *
	 * @return false if the lever is unknown, its BaselineSource is not CapturedOnce, it has no cvar,
	 *         or FPM currently holds it (logged as an Error in every refusal case).
	 */
	bool CaptureBaselineOnce(FName LeverName);

	/**
	 * ★ THE CLASSIFIER LIVENESS PROOF, run once from Arm(). Registers the self-test fixture levers
	 * and proves, against real known-positive/known-negative pairs (never invented ones -- see the
	 * .cpp), that: (1) the US_* / clause-2 refusal actually refuses a real backed/sg.* name AND does
	 * NOT wrongly refuse a real safe one (the "correct lever wrongly refused" mirror check); (2) the
	 * capability-probe classifier can tell a present cvar from an absent one; (3) the alias table
	 * resolves a real BaseScalability.ini member and returns nothing for a group that does not
	 * exist; (4) the anti-ratchet guard actually refuses a capture against a held cvar and actually
	 * succeeds (once, idempotently) against one it does not hold.
	 *
	 * @return true only if every check passed. `ReportNow` REFUSES to print a coverage table when
	 *         this is false, matching FPMServerLevers' "do not publish a table of confident zeros".
	 */
	bool SelfTest();

	/** `FPM.Lever.Report` -- prints registration + probe coverage. Refuses (logs why, prints
	 *  nothing else) if the self-test has not passed. */
	void ReportNow(FOutputDevice& Ar) const;

	/**
	 * How many registry entries are SELF-TEST FIXTURES, and how many are shipped levers. Review
	 * 2026-08-15, M2: the fixtures live in the same table, so every count printed anywhere must say
	 * which it is counting. Derived from FPMLeverSelfTestPrefix at registration, so a new fixture
	 * cannot be miscounted by an author who forgot a flag.
	 */
	int32 CountSelfTestFixtures() const;
	int32 CountProduction() const;

private:
	bool RefuseIfUnsafeToWrite(const FFPMLeverDefinition& Def, FString& OutReason) const;
	void ProbeOne(FFPMLeverDefinition& Def) const;
	void RegisterSelfTestLevers();

	TMap<FName, FFPMLeverDefinition> Levers;

	/** ONE TIER OF ONE SCALABILITY GROUP. Names and values are built together, in one pass over one
	 *  config section, so the two views of the same tier cannot disagree. `Names` keeps the section's
	 *  own order (a TMap would not), because a tier's members are applied in the order the game's own
	 *  table lists them. */
	struct FAliasTier
	{
		TArray<FString> Names;
		TMap<FString, FString> Values;
	};

	/** GroupName -> Tier -> that tier's members and their prescribed values. */
	TMap<FString, TMap<int32, FAliasTier>> AliasTable;

	/** Name + reason, for refused registrations. The definitions themselves are never stored --
	 *  see RegisterWritable's comment. */
	TArray<FString> RefusedRegistrations;

	bool bSelfTestPassed = false;
};
