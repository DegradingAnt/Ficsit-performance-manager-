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

	/**
	 * ★ NON-EMPTY WHEN THIS LEVER'S BEHAVIOUR DEPENDS ON A RULING ANT HAS NOT GIVEN.
	 *
	 * The value is the ruling id, for example "policy-gap" or "K4f-depth". Every report line and
	 * every dry-walk line that shows such a lever prints an AWAITING RULING marker, and the count of
	 * marked levers is printed with it. A marker instrument that can silently reach zero is a dead
	 * instrument, so the count is the coverage denominator for the marker.
	 *
	 * ⚠ THIS FIELD CHANGES NO BEHAVIOUR. It labels the shipped value; it does not replace it. The
	 * lever moves exactly as it moved before this field existed.
	 */
	FString AwaitingRuling;
};

/**
 * ★ DOES A HIGHER NUMBER MEAN BETTER QUALITY FOR THIS CVAR?
 *
 * Not a property anyone can look up: r.Nanite.MaxPixelsPerEdge gets WORSE as it rises and
 * r.LumenScene.GlobalSDF.Resolution gets BETTER. The tables answer it themselves, because the design
 * header states that MaxOf and MinOf "never worsen a user's own baseline" (:304). So a BONUS lever
 * that uses MaxOf proves higher is better for its cvar, and a bonus lever that uses MinOf proves the
 * opposite. Unknown is a first-class answer: a cvar whose only bonus lever is Absolute casts no vote,
 * and a guess there would be an invention.
 */
enum class EFPMLeverPolarity : uint8
{
	Unknown,
	HigherIsBetter,
	LowerIsBetter,
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMLeverPolarity Polarity);

/** ONE LEVER PLUS THE TIER IT SITS IN, flat. The invariant checks below take a flat array so a
 *  self-test can hand them a DELIBERATELY VIOLATED copy of the shipped tables and prove the check
 *  refuses it. A check that only ever sees data it passes is indistinguishable from a constant. */
struct FICSITSPERFORMANCEMANAGER_API FFPMFlatLever
{
	EFPMStageTier  Tier = EFPMStageTier::None;
	FFPMStageLever Lever;
};

/**
 * ★ THE DRY PROJECTION. Applies one lever's POLICY ARITHMETIC to a baseline the CALLER supplies, and
 * returns the value the lever would land on. It reads no console variable, it writes no console
 * variable, and it touches FPMCVarWriter not at all.
 *
 * ⚠ THIS IS NOT THE APPLY PASS, and the difference is the baseline. The apply pass needs a baseline
 * from the shipped vanilla-defaults table (Law 3), and no such table exists yet for engine r.* cvars.
 * This function refuses to have an opinion about where the baseline came from: it is a parameter. The
 * dry walk passes a declared SYNTHETIC baseline, and the direction invariant passes a SWEEP of
 * baselines, because an invariant that holds at one baseline and not another is not an invariant.
 *
 * Group levers return the baseline unchanged and set OutNote, because a group lever's target is a
 * TIER and not a value. Use FFPMStageTables::ResolveGroupTarget for those.
 *
 * ★ OutBeforeClamp SEPARATES THE STEP FROM THE RANGE GUARD. They are two different facts and one
 * number cannot carry both. The RETURN VALUE is where the lever LANDS, clamp included. OutBeforeClamp
 * is what the lever's own POLICY ARITHMETIC produced, before the clamp had a say.
 *
 * A caller judging the STEP'S DIRECTION must read OutBeforeClamp. For a baseline outside the lever's
 * declared clamp band it is the CLAMP, not the policy, that decides the landing, and scoring the
 * clamp's correction as a quality step reports the exact opposite of the truth. That is measured, not
 * hypothetical: it is why self-test (9) accused K3's r.Lumen.ScreenProbeGather.DownsampleFactor of
 * IMPROVING a value on a cut tier at baseline 100 (100 * 2.0 = 200, clamped back to 32, which reads
 * as a fall). The engine's own help text for that cvar -- "Pixel size of the screen tile that a
 * screen probe will be placed on", read out of the shipped Renderer module -- says the K3 step raises
 * it, which makes the tile bigger, the probes sparser and the frame cheaper. The step was right and
 * the check was reading the clamp.
 *
 * The game's own table agrees, independently: BaseScalability.ini sets this cvar to 32 at
 * GlobalIlluminationQuality@2, 16 at @3 and 8 at @Cine, so it FALLS as quality RISES. That is also
 * where K3's [8,32] clamp band comes from -- it is the cvar's own reachable range in this game, which
 * is exactly why a swept baseline of 100 or 4096 is an input that cannot occur and must not be
 * allowed to decide a direction verdict.
 */
FICSITSPERFORMANCEMANAGER_API float FPMProjectLeverValue(const FFPMStageLever& Lever,
                                                         float AssumedBaseline,
                                                         FString& OutNote,
                                                         float* OutBeforeClamp = nullptr);

/** Print a lever value the way a cvar carries it: whole numbers stay whole, the rest keep 4 places
 *  with trailing zeros trimmed. "0.6" and "0.600000" must not read as two different truths. */
FICSITSPERFORMANCEMANAGER_API FString FPMFormatLeverValue(float Value);

/**
 * ★ WHY A GROUP LEVER'S ALIAS MEMBER MAY BE UNWRITABLE. See FPMClassifyGroupMember.
 */
enum class EFPMGroupMemberExclusion : uint8
{
	/** Nothing bars it. A group step may write this member. */
	None,

	/** Section 3.4's one floor law. r.Lumen.DiffuseIndirect.Allow is the GI kill switch, and no FPM
	 *  path may name it -- not to set it to 0, and not to set it to 1 either. A hold on a kill switch
	 *  is one edit away from a hold at the wrong value, which is why the ban is categorical. */
	ForbiddenGICVar,

	/** Law 1. FGGameUserSettings serialises every mUserSettings entry on every save with no dirty
	 *  gate, so an FPM write to a US_*-backed cvar becomes the player's PERMANENT setting and
	 *  survives uninstall. */
	UserSettingBacked,
};

/**
 * ★ THE ONE ANSWER TO "MAY A GROUP STEP WRITE THIS MEMBER CVAR", FOR EVERY BRANCH THAT ASKS.
 *
 * A ScalabilityGroup lever never writes the group name; it writes the group's ALIAS MEMBERS. Two
 * separate code paths expand a group into members -- FFPMStageTables::UnderlyingCVars (what the
 * ladder can reach) and FFPMStageTables::DeriveK4gMembers (what K4g would arm) -- and until this
 * function existed they each carried their own copy of the exclusion rule. K4g's copy excluded the GI
 * kill switch; UnderlyingCVars' copy did not exist at all, so the kill switch was reachable from the
 * B5 and K3 group levers through GlobalIlluminationQuality@2, and self-test (8) caught it.
 *
 * That is this project's recorded law that a fix in one branch is not a fix. There is now ONE rule
 * and both branches call it, so the two cannot drift apart again.
 */
FICSITSPERFORMANCEMANAGER_API EFPMGroupMemberExclusion FPMClassifyGroupMember(const FString& Member);

/**
 * ★ THE INVARIANT CHECKS, AS FREE FUNCTIONS OVER DATA THEY ARE GIVEN.
 *
 * Every one of them is a PROJECT LAW rather than a preference, and every one takes the thing it
 * checks as a parameter for one reason: so the self-test can run it twice, once over the shipped
 * tables (which must PASS) and once over a copy mutated to break the law (which must FAIL). The
 * second half is the only thing that can tell a working check apart from `return true`.
 */
namespace FPMStageInvariants
{
	/** Resolver signature: (lever, live group tier, out note) -> the tier it lands on, or INDEX_NONE
	 *  when the step is refused. The shipped resolver is FFPMStageTables::ResolveGroupTarget; the
	 *  self-test also passes an UNCLAMPED one, which must be caught. */
	using FGroupResolver = TFunctionRef<int32(const FFPMStageLever&, int32, FString&)>;

	/**
	 * LAW: the GlobalIlluminationQuality group never moves below @2 on any FPM path (design section
	 * 3.4). Sweeps every reachable live tier against every group lever given, and fails if any
	 * resolved target lands under FloorTier.
	 *
	 * OutCasesChecked is the coverage denominator. Zero cases means the check proved nothing, and the
	 * caller must treat that as a failure rather than as a pass.
	 */
	FICSITSPERFORMANCEMANAGER_API bool GIFloorNeverBreached(const TArray<FFPMFlatLever>& GroupLevers,
	                                                        FGroupResolver Resolver,
	                                                        int32 FloorTier,
	                                                        FString& OutFailure,
	                                                        int32& OutCasesChecked);

	/**
	 * LAW: r.Lumen.DiffuseIndirect.Allow never appears on any ladder. @0 and @1 both carry it at 0,
	 * and that is the 0.52.0 lighting regression this project has now refused three separate times.
	 *
	 * Takes the union of every cvar every tier of every mode order can touch. A caller must prove
	 * that union is not empty before believing a pass; PositiveControl is the name that must be
	 * present, and the check fails if it is not, so an empty extractor cannot report a clean bill.
	 */
	FICSITSPERFORMANCEMANAGER_API bool ForbiddenGICVarAbsent(const TSet<FString>& LadderCVars,
	                                                         const TCHAR* PositiveControl,
	                                                         FString& OutFailure);

	/** LAW: every tier an order names is a real tier. An EMPTY tier is legitimate and must not fail
	 *  this check: K4g derives to the empty set on this engine build and is still correctly named. */
	FICSITSPERFORMANCEMANAGER_API bool OrderNamesRealTiers(const TArray<EFPMStageTier>& Order,
	                                                       const TCHAR* Label,
	                                                       FString& OutFailure);

	/**
	 * LAW: no bonus step worsens a lever, and no cut step improves one.
	 *
	 * Derives each cvar's polarity from the BONUS levers in the array it is given (see
	 * EFPMLeverPolarity), then projects every lever across a baseline sweep and checks the direction.
	 * Two K3 levers are exempt by name, each with a stated reason, and they are COUNTED so the
	 * exemption list cannot grow unnoticed.
	 *
	 * ★ IT JUDGES TWO THINGS, NOT ONE, BECAUSE A CLAMP IS NOT A STEP.
	 *   (a) THE STEP, read before the clamp. This is the law as written: a bonus never worsens and a
	 *       cut never improves. It holds at every baseline in the sweep, in range or out of it,
	 *       because policy arithmetic has no range.
	 *   (b) THE LANDING, read after the clamp, and ONLY for baselines INSIDE the lever's own declared
	 *       clamp band. A clamp exists to pull an out-of-band value back into the band; doing that is
	 *       the clamp obeying its contract, not the tier changing quality, and judging it as a step
	 *       reports the opposite of the truth. Inside the band the clamp's contract does apply, so a
	 *       clamp that flips the landing there is a real defect and is still caught.
	 *
	 * Before the split there was one verdict over the post-clamp value, and it produced a FALSE
	 * ACCUSATION against a correct lever (K3 r.Lumen.ScreenProbeGather.DownsampleFactor) while proving
	 * nothing extra. Widening what the check can see is the point; neither half was removed.
	 *
	 * OutChecked / OutNoPolarity / OutExempt are the coverage numbers, and the sweep RUNS TO COMPLETION
	 * so they mean what they say. The old early return on the first failure left OutExempt reading 0
	 * while two exemptions existed and were printed one line later, which is a count that contradicts
	 * the list beside it. The FIRST failure is still the one reported.
	 *
	 * OutInBandLandingCases is law (b)'s OWN coverage. The shared baseline sweep sits outside every
	 * clamp band and for a narrow band misses it entirely, so a clamped lever is additionally swept at
	 * its band's ends and middle. A zero here would mean law (b) judged nothing at all, which reads
	 * exactly like law (b) passing.
	 */
	FICSITSPERFORMANCEMANAGER_API bool StepDirectionsSane(const TArray<FFPMFlatLever>& Levers,
	                                                      FString& OutFailure,
	                                                      int32& OutChecked,
	                                                      int32& OutNoPolarity,
	                                                      int32& OutExempt,
	                                                      int32& OutInBandLandingCases);

	/** Polarity of one cvar, derived from the bonus levers in the array given. Exposed so a report
	 *  can print it beside a lever rather than a reader having to trust the check. */
	FICSITSPERFORMANCEMANAGER_API EFPMLeverPolarity DerivePolarity(const TArray<FFPMFlatLever>& Levers,
	                                                               const FString& CVarName);

	/** True when the two lists hold the same names, ignoring order. The comparator behind the
	 *  no-write proof: it must be able to say NO, so its own liveness check feeds it a perturbed
	 *  copy and requires a false. */
	FICSITSPERFORMANCEMANAGER_API bool SameNameSet(const TArray<FString>& A, const TArray<FString>& B,
	                                               FString& OutDelta);
}

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

	/** Every registered lever with its tier, flat. The input the invariant checks above take, and the
	 *  thing a self-test copies and mutates to build a known-bad case. */
	void FlattenLevers(TArray<FFPMFlatLever>& Out) const;

	/**
	 * The union of every underlying cvar every tier of every mode order can touch, plus the derived
	 * K4g members. This is what the forbidden-cvar law is checked against, and its SIZE is the
	 * coverage denominator for that check.
	 *
	 * ★ OutGIKillSwitchExclusions IS THE COVERAGE DENOMINATOR FOR THE FILTER ITSELF, and it exists
	 * because a working filter and a DEAD GROUP EXPANSION produce the same clean verdict. It counts
	 * how many times the group expansion met r.Lumen.DiffuseIndirect.Allow and dropped it. Zero is not
	 * automatically wrong -- a future BaseScalability.ini might stop carrying it at @2/@3 -- but zero
	 * must be SAID, because "the law holds" and "nothing was examined" have to be told apart.
	 */
	void CollectLadderCVars(TSet<FString>& Out, int32* OutGIKillSwitchExclusions = nullptr) const;

	/** The cvar the GI floor law exists to keep off every ladder. One declaration site. */
	static const TCHAR* ForbiddenGICVarName();

	/** K4g's derived member names, and the sentence that says how the derivation went. Empty on this
	 *  engine build by construction; see DeriveK4gMembers. */
	const TArray<FString>& GetK4gMembers() const { return K4gMembers; }
	const FString& GetK4gDerivationNote() const { return K4gDerivationNote; }

	/** One line per lever whose behaviour depends on a ruling Ant has not given, each already
	 *  carrying the AWAITING RULING marker. The count is printed with them, because a marker list
	 *  that quietly reaches zero looks exactly like a list of settled questions. */
	void GetAwaitingRulings(TArray<FString>& Out) const;

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
	 *
	 * Then the four PROJECT LAWS, each run twice: once over the shipped tables, which must PASS, and
	 * once over a copy mutated to break the law, which must FAIL.
	 *   5. Every tier an order names is a real tier, and an EMPTY tier is legitimate. The known-bad
	 *      copy carries EFPMStageTier::None; the empty-tier half is proven by K4g, which every mode
	 *      names and which carries no levers on this engine build.
	 *   6. The GlobalIlluminationQuality group never moves below @2 on any path. The known-bad case
	 *      is an UNCLAMPED resolver, which is the arithmetic the clamp replaced.
	 *   7. r.Lumen.DiffuseIndirect.Allow never appears on any ladder. The known-bad case is the same
	 *      cvar set with that name injected, and a positive control stops an empty extractor from
	 *      reporting a clean ladder. The law is kept true at the GROUP EXPANSION, by
	 *      FPMClassifyGroupMember, so the ladder genuinely cannot write the kill switch -- and the
	 *      number of times that exclusion fired is printed, because a working filter and a dead alias
	 *      table both produce a clean ladder.
	 *   8. No bonus step worsens a lever and no cut step improves one. Judged as TWO laws: the STEP
	 *      (the policy's arithmetic, before the clamp) across the whole sweep, and the LANDING (after
	 *      the clamp) only for baselines inside the lever's own clamp band. One merged verdict over
	 *      the post-clamp value falsely accused a correct K3 lever, because a clamp pulling an
	 *      out-of-band baseline home is the clamp working, not the tier changing quality. The
	 *      known-bad case flips one K1 lever's policy. Two K3 levers are exempt by name, both are
	 *      printed with their reasons, and abstentions are counted rather than passed.
	 *   9. The AWAITING RULING marker is not dead: a count of zero fails, because a marker list that
	 *      reaches zero reads exactly like a list of settled questions.
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
	 *  of its alias members across the REACHABLE tiers (@2 to @3; @0/@1 are unreachable under §3.4),
	 *  MINUS the GI kill switch, which no FPM path may name. OutGIKillSwitchExclusions accumulates
	 *  (it is not reset) how many members that exclusion dropped -- see CollectLadderCVars for why the
	 *  count is not optional information. */
	void UnderlyingCVars(EFPMStageTier Tier, TSet<FString>& Out,
	                     int32* OutGIKillSwitchExclusions = nullptr) const;

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
