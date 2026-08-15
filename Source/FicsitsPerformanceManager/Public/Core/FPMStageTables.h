// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMLeverTypes.h"

/**
 * ★ SLICE 2 -- THE STAGE TABLES. Design §14's Slice-2 bullet "stage tables with the K3 clamp and the
 * K4g/K4f split", content from §3.2 (FPM2-DESIGN-ASSEMBLED.md:304-338).
 *
 * This file is the CONTENT half of M-LEVER. FPMLeverRegistry is the MECHANISM half and shipped with
 * five self-test fixtures and no production levers; this file is the thing its header calls "a future
 * stage-table file [that] registers real levers through the SAME RegisterWritable/RegisterReadOnly
 * API" (FPMLeverRegistry.h:23-28). Read that header first.
 *
 * WHY THE TARGET VALUES LIVE HERE AND NOT IN FFPMLeverDefinition
 *
 * The registry's definition struct carries no value field, deliberately: it answers "may FPM write
 * this at all, and does the subsystem exist here", never "what should it be set to". A lever's target
 * VALUE is a steering decision, and steering decisions belong to the tables and the walk. So each
 * stage lever is stored twice on purpose: an FFPMStageLever here (value, clamp, VRAM gate) and a
 * matching FFPMLeverDefinition in the registry (safety enforcement, capability probe). The registry
 * key is the join, and it is the registry's copy that can REFUSE. If it refuses, this file never gets
 * to write the value, which is the point.
 *
 * ⚠ WHAT THIS FILE DOES NOT DO
 *
 * It does not decide WHEN a tier moves (FPMGiveTake.h), it does not APPLY a value (see FPMGiveTake.h's
 * own note on why the apply pass is not built yet), it does not own the resolution lever or R+ (§8,
 * a Slice-3 investigation), and it does not select a mode (§14's "modes" bullet, with its config keys
 * and UI states). It owns: the lever content, the per-mode give/take ORDERS, the GI floor clamp, the
 * K4g derivation, and the order self-check that proves the orders are internally consistent.
 */

/**
 * ★ TIER IDENTITY. §3.3 (:352, citing FPM-DESIGN-MODES-2026-08-02.md:161-168): "costs/cooldowns key by
 * TIER IDENTITY, not ladder position". An enum rather than an index is what makes that true rather
 * than merely intended, because mode C moves K4f three places and every cooldown must follow the tier.
 *
 * K4 IS TWO TIERS (design :285-287, fixing review finding H4). K4g is the GI member cut; K4f is the
 * foliage cut. They are separate values here because the mode orders genuinely separate them.
 */
enum class EFPMStageTier : uint8
{
	None = 0,

	/** Bonus tiers, design :306-312, printed there as "+1" through "+6". */
	B1, B2, B3, B4, B5, B6,

	/**
	 * The resolution lever's ORDERED POSITION in every mode's walk. It carries NO levers in this file:
	 * resolution is owned by §8 (native dyn-res, or the active upscaler's rung ladder), which this
	 * slice does not build. It exists as a tier identity because its POSITION in the order is what
	 * dissolves the 0.55.0 deadlock. See FPMGiveTake.h's "how the deadlock is structurally gone".
	 */
	Resolution,

	/** Cut tiers, design :320-336. */
	K1, K2, K3, K4g, K4f,

	Count
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMStageTier Tier);
FICSITSPERFORMANCEMANAGER_API bool FPMIsBonusTier(EFPMStageTier Tier);
FICSITSPERFORMANCEMANAGER_API bool FPMIsCutTier(EFPMStageTier Tier);

/** §3.2's four modes (:273-278). Balanced is not selectable; it is the pre-bench state (§3.5a). */
enum class EFPMGovernorMode : uint8
{
	Balanced = 0,
	ResolutionFirst,   // A
	GraphicsFirst,     // B
	LightingFirst,     // C
	Count
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMGovernorMode Mode);

/**
 * ONE LEVER INSIDE A TIER. Plain data, the same shape choice FFPMLeverDefinition makes.
 */
struct FICSITSPERFORMANCEMANAGER_API FFPMStageLever
{
	/** The key this lever is registered under in FFPMLeverRegistry. The join between the two copies. */
	FName RegistryKey;

	/** Exactly one of CVarName / GroupName is set. */
	FString CVarName;
	FString GroupName;

	/** Only meaningful for a group lever: +1 for B5, -1 for K3. The GI floor clamp is applied by
	 *  FFPMStageTables::ResolveGroupTarget, never by a caller doing the arithmetic itself. */
	int32 GroupStep = 0;

	/** The value the design's table names. Text, because a cvar is text at the writer's boundary and
	 *  because "0.6" and "0.6f" must not become two different truths. Empty for a group lever. */
	FString TargetValue;

	EFPMLeverPolicy Policy = EFPMLeverPolicy::Absolute;

	/** BaseScale / BaseDelta results are clamped to this range where the design states one. K3 gives
	 *  two: DownsampleFactor [8,32] and TraceDistanceScale [0.25,1] (:330). */
	bool bHasClamp = false;
	float ClampMin = 0.0f;
	float ClampMax = 0.0f;

	/** The design's "@11.5GB" / "@15GB" annotations, in MB, as a capability gate rather than a comment.
	 *  0 means no VRAM gate. The 15GB tier gates at 15000 and not 15500 because a 16GB card reports
	 *  about 15209MB (design :312, carried receipt). */
	int64 VramGateMB = 0;

	/** Free text carried onto the report line. Where a policy or a value is INFERRED rather than
	 *  stated by the design, this says so. That is the difference between a carried table and an
	 *  invented one. */
	FString Note;
};

class FICSITSPERFORMANCEMANAGER_API FFPMStageTables final : public IFPMFix
{
public:
	static FFPMStageTables& Get();

	virtual const TCHAR* Name() const override { return TEXT("stage-tables"); }

	/** GPU-side quality levers. None of them means anything on a dedicated server, which has no
	 *  renderer. It is the same answer every lever definition here gives individually. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** UnknownCause, for the reason FPMServerLevers.h:63-68 and FPMLeverRegistry.h:55-58 both give:
	 *  nothing here is being FIXED. The value is a catalogue plus the checks that keep it honest. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Steering; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;
	virtual void Disarm() override;

	/** The levers of one tier, in the order the design lists them, which is the order they are applied
	 *  in. For K3 that matters: the GROUP step is first, then the hand levers (:330). */
	const TArray<FFPMStageLever>& LeversIn(EFPMStageTier Tier) const;

	/** The walk order under load, and its exact reverse for the take side. */
	const TArray<EFPMStageTier>& GiveOrder(EFPMGovernorMode Mode) const;
	const TArray<EFPMStageTier>& TakeOrder(EFPMGovernorMode Mode) const;

	/**
	 * How many of this tier's levers the registry's probe pass found AVAILABLE on this machine.
	 *
	 * ★ THIS IS THE COVERAGE DENOMINATOR, and it is the reason a caller must never assume a tier does
	 * something. Returns 0 before OnWorldLoad has run (nothing is probed yet) and 0 for a tier whose
	 * every lever is absent or refused.
	 */
	int32 AvailableLeverCount(EFPMStageTier Tier) const;

	/**
	 * True when moving this tier could not change anything, with the reason in plain words.
	 *
	 * ⚠ K4g IS INERT BY CONSTRUCTION ON THIS ENGINE BUILD, and that is a measured fact rather than a
	 * defensive branch. See DeriveK4gMembers in the .cpp for the set arithmetic and the receipt.
	 */
	bool IsTierInert(EFPMStageTier Tier, FString& OutReason) const;

	/**
	 * ★ THE GI FLOOR CLAMP (§3.4's one floor law, §3.2's K3 clamp at :330).
	 *
	 * Returns the scalability tier a group lever would land on, clamped so the GlobalIlluminationQuality
	 * group never goes below @2 on any FPM path. Returns INDEX_NONE when the step is refused by the
	 * clamp, and writes the reason. The walk then proceeds to the next lever rather than stalling,
	 * which is the design's own wording: "it reports 'at GI floor, skipped' and the walk proceeds".
	 *
	 * `LiveTier` is read from the group's own cvar by the caller. Reading it is safe under Law 3: FPM
	 * never WRITES the group (FPMCVarWriter clause 2 refuses every sg.* write), so the value can never
	 * be our own prior write coming back, which is the ratchet the law exists to stop.
	 */
	int32 ResolveGroupTarget(const FFPMStageLever& Lever, int32 LiveTier, FString& OutNote) const;

	/** The GI group's floor. One declaration site for §3.4's number. */
	static int32 GIGroupFloorTier() { return 2; }
	/** The highest non-cinematic tier. The alias table deliberately stops before Cine. */
	static int32 GroupCeilingTier() { return 3; }

	/**
	 * ★ THE ARM-TIME ORDER GATE (§3.2 :291-295). Mode C reorders tiers relative to the canonical A/B
	 * order; the design permits only reorders "that the ALIAS TABLE clears", meaning no reordered PAIR
	 * may share an underlying cvar.
	 *
	 * ⚠ IT COMPARES PAIRS THAT MOVED, NOT ADJACENT PAIRS, and that distinction is the whole check. A
	 * naive "no two neighbouring tiers share a cvar" rule REFUSES THE SHIPPED TABLES on their first
	 * run: B5's group members and B6's hand levers legitimately share
	 * r.Lumen.ScreenProbeGather.DownsampleFactor while sitting next to each other in every mode. The
	 * hazard is not sharing; it is sharing across a pair whose ORDER changed, because then one mode
	 * applies X-then-Y and another Y-then-X over the same underlying value.
	 *
	 * @return true if every mode's order is clear. On false, OutFailure names the mode and the pair.
	 */
	bool CheckOrderInversions(FString& OutFailure) const;

	/**
	 * ★ THE LIVENESS PROOF. Every check below has a known-positive AND a known-negative, because four
	 * gates in this project shipped able to refuse correct work and nothing noticed:
	 *   1. The GI clamp REFUSES a step from @2 to @1 and ALLOWS a step from @3 to @2. A clamp that
	 *      only ever refuses is the same defect as a gate that only ever passes.
	 *   2. CheckOrderInversions PASSES the shipped orders and FAILS a synthetic order that swaps a
	 *      pair which really does share an underlying cvar. Without the second half this check could
	 *      be returning a constant true and nothing would show it.
	 *   3. Take order is the exact reverse of give order for every mode, verified element by element
	 *      rather than asserted in a comment.
	 *   4. Every registered stage lever survived the registry's refusal (Law 1 / clause 2). A tier
	 *      that lost levers to a refusal reports it rather than shrinking quietly.
	 */
	bool SelfTest();

	/** `FPM.Stage.Report` -- the tables, their probe coverage, and every inert tier with its reason.
	 *  Refuses to print if the self-test failed, matching FPMServerLevers and FPMLeverRegistry. */
	void ReportNow(FOutputDevice& Ar) const;

private:
	void BuildOrders();
	void RegisterTables();
	void DeriveK4gMembers();
	void RegisterOne(EFPMStageTier Tier, FFPMStageLever Lever, EFPMLeverCurrency Currency,
	                 const TCHAR* EstimatedCost, const TCHAR* Provenance);

	/** Every underlying cvar a tier touches: a cvar lever's own name, and for a group lever the union
	 *  of its alias members across the REACHABLE tiers (@2 to @3; @0/@1 are unreachable under §3.4). */
	void UnderlyingCVars(EFPMStageTier Tier, TSet<FString>& Out) const;

	TArray<FFPMStageLever> TierLevers[static_cast<int32>(EFPMStageTier::Count)];
	TArray<EFPMStageTier>  GiveOrders[static_cast<int32>(EFPMGovernorMode::Count)];
	TArray<EFPMStageTier>  TakeOrders[static_cast<int32>(EFPMGovernorMode::Count)];

	/** Names the registry refused, kept so a shrunken tier is visible rather than silent. */
	TArray<FString> RefusedByRegistry;

	/** Filled at world load from the live alias table. Empty on this engine build, by construction. */
	TArray<FString> K4gMembers;
	FString K4gDerivationNote;

	bool bSelfTestPassed = false;
	bool bProbed = false;
};
