// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Misc/EnumClassFlags.h"

#include "Core/FPMFixContract.h"   // EFPMFixSide -- a lever's side reuses the fix contract's own
                                    // enum rather than a second one that could drift from it.

/**
 * ★ THE LEVER REGISTRY'S DATA MODEL. Design §9.1 (FPM2-DESIGN-ASSEMBLED.md:1250-1253): "every
 * tunable is a lever with {cvar(s) or CDO target, policy, tier/band, side, gates, capability probe
 * (§3.12), provenance}, plus the ALIAS TABLE mapping group levers to their member cvars (§3.2)."
 *
 * THIS FILE IS THE SCHEMA ONLY. The registry that enforces it lives in FPMLeverRegistry.h; the
 * decisions about WHICH real levers exist and WHEN they move (the stage tables, the give/take
 * walk, the gate terms "at floor / GPU-bound / bench-worthwhile") are a later, separate item --
 * §14 lists "stage tables with the K3 clamp" as its own Slice-2 bullet, distinct from "writer-
 * backed lever registry with capability probes and the alias table". This header builds the
 * second one only.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * WHERE EACH TUPLE FIELD LANDS, AND WHERE IT DELIBERATELY DOES NOT
 *
 * "cvar(s)"        -> CVarNames / GroupName, per EFPMLeverBacking.
 * "policy"         -> EFPMLeverPolicy, the vocabulary carried from FPM1's evolved tables.
 * "tier/band"      -> TierHint, free text ("K3", "+2"). Catalogue metadata ONLY -- it drives no
 *                      behaviour. The stage tables own tier ASSIGNMENT and the walk order; this
 *                      field exists so a lever can be filed against its eventual tier without the
 *                      registry pretending to implement the ladder.
 * "side"           -> Side, reusing EFPMFixSide.
 * "gates"          -> NOT implemented here. §3.2's gate terms ("at floor", "GPU-bound", "mean-
 *                      over-budget-at-floor", "bench-worthwhile") are steering conditions -- WHEN a
 *                      lever is allowed to move -- not structural facts about the lever itself.
 *                      That is the governor's job. CapabilityProbe below answers a different
 *                      question ("CAN this lever exist on this machine at all") and must not be
 *                      confused with a steering gate.
 * "capability probe" -> CapabilityProbe, §3.12.
 * "provenance"     -> Provenance (free text) for where the DEFINITION came from (carried FPM1
 *                      table, new, etc). This is NOT the bench's per-measurement provenance enum
 *                      (Default/Measured/Measured-Unpinned/UserOverride/VendorClamp/Unavailable,
 *                      §4.4) -- that describes a COST MEASUREMENT's origin and belongs to M-BENCH,
 *                      a different subsystem this slice does not build.
 * "currency"       -> Currency (EFPMLeverCurrency flags) + EstimatedCost (free text). The task
 *                      vocabulary's currency axes (gpu_ms / vram_mb / cpu_ms). Declared as AXES a
 *                      lever spends in, not a fabricated per-lever number -- the carried FPM1
 *                      tables only ever cost a whole TIER (e.g. "+2 * 0.8ms" across ~6 cvars), and
 *                      per-lever costs are the bench's job to MEASURE, not this registry's job to
 *                      invent.
 * ALIAS TABLE      -> owned by FPMLeverRegistry (a runtime table, not a per-lever field).
 */

/** MaxOf/MinOf never worsen a user's own baseline; Absolute overrides; BaseScale/BaseDelta scale
 *  or offset the baseline; ScalabilityGroup moves a CSS scalability group tier, relatively.
 *  Carried verbatim from FPM2-DESIGN-ASSEMBLED.md:316-317 -- the vocabulary already has an owner. */
enum class EFPMLeverPolicy : uint8
{
	MaxOf,
	MinOf,
	Absolute,
	BaseScale,
	BaseDelta,
	ScalabilityGroup,
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMLeverPolicy Policy);

/**
 * ★ WHAT A LEVER SPENDS. Task vocabulary: "CURRENCY: what a lever costs. gpu_ms, vram_mb, cpu_ms."
 *
 * Flags, not a single value, because a lever can spend on more than one axis at once (a VRAM-
 * gated Lumen tier spends both GpuMs and VramMb -- see the "+3"/"+6" bonus tiers' "@11.5GB" /
 * "@15GB" gates in the design's carried tables). `None` is a legal, if unusual, declaration for a
 * lever whose cost has not been characterised at all yet.
 */
enum class EFPMLeverCurrency : uint8
{
	None   = 0,
	GpuMs  = 1 << 0,
	VramMb = 1 << 1,
	CpuMs  = 1 << 2,
};
ENUM_CLASS_FLAGS(EFPMLeverCurrency)
FICSITSPERFORMANCEMANAGER_API FString LexToString(EFPMLeverCurrency Currency);

/**
 * WHAT A LEVER ACTUALLY WRITES.
 *
 * `Cvar` covers the overwhelming majority of the carried tables. `ScalabilityGroup` is its own
 * kind rather than folded into Cvar because the write NEVER lands on the group name itself --
 * FPMCVarWriter's clause 2 refuses every `sg.*` write unconditionally (FPMCVarWriter.cpp:22-23,
 * comment: "The ladder expands a group into its member cvars and drives the members; it never
 * writes the group") -- so a ScalabilityGroup-backed lever's real writes are its ALIAS TABLE
 * members, resolved through FPMLeverRegistry::GetAliasMembers, never `GroupName` directly.
 *
 * ⚠ CDO IS NAMED IN THE DESIGN TUPLE AND DELIBERATELY NOT A THIRD VALUE HERE. Every lever in every
 * carried table (§3.2) is cvar- or scalability-group-backed; none targets a CDO. Adding an
 * `EFPMLeverBacking::Cdo` today would be a write path with nothing real to register against it and
 * nothing to test it with -- an untested abstraction is worse than an honest gap. The day a real
 * CDO-backed lever is designed, it earns its own value AND its own write path (CDO mutation does
 * not go through FPMCVarWriter at all, so it needs a parallel enforcement story, not a bolt-on).
 */
enum class EFPMLeverBacking : uint8
{
	Cvar,
	ScalabilityGroup,
};

/**
 * ★ LAW 3, ENFORCED BY WHAT THIS ENUM DOES NOT OFFER. "Baselines come from the shipped vanilla-
 * defaults table, never from a live cvar read. A live read may be our own previous write re-
 * applied by the game, which makes the baseline RATCHET."
 *
 * There is no "read it live, whenever" value in this enum. That is deliberate, mirroring Law 1's
 * own shape (a US_*-backed lever cannot be registered writable because the registry offers no path
 * to do so) applied to the ratchet hazard instead: a policy that needs a baseline (MaxOf / MinOf /
 * BaseScale / BaseDelta) must declare one of the two SAFE sources below, or FPMLeverRegistry::
 * RegisterWritable refuses it outright (FPMLeverRegistry.cpp, RefuseIfUnsafeToWrite). The unsafe
 * third option is not merely undocumented -- it does not exist to select.
 */
enum class EFPMLeverBaselineSource : uint8
{
	/** Absolute / ScalabilityGroup policy: nothing to compare against, no baseline needed. */
	NotApplicable,

	/** Ships in a compiled table -- e.g. a BaseScalability.ini tier read at a KNOWN, un-steered
	 *  tier (never the live/current tier, which could already reflect our own prior write). A
	 *  cvar-level table in the FPMUserSettingTable.g.h shape (the US_* asset table) does not exist
	 *  yet for engine r.* cvars; wiring this source is future work, not invented here. */
	ShippedTable,

	/** No shipped table exists for this cvar. Captured exactly ONCE, before FPM's first Hold of it
	 *  THIS SESSION, and never re-derived afterward -- FPMLeverRegistry::CaptureBaselineOnce is the
	 *  only way to populate it, and it refuses outright if FPMCVarWriter already holds the cvar. */
	CapturedOnce,
};

/**
 * ★ THE CAPABILITY PROBE'S OUTCOME (§3.12). NOT a steering gate -- see the file header's "gates"
 * note. This says whether the lever's subsystem exists on THIS machine/build at all.
 */
enum class EFPMLeverAvailability : uint8
{
	/** Not probed yet -- true before OnWorldLoad's probe pass has run once. */
	Unknown,

	/** The probe passed. */
	Available,

	/** The probe failed -- "lever ABSENT", the §3.12 / FPM1 pattern, kept. */
	Absent,

	/** Registration itself was refused (US_*-backed, direct sg.* write, or an unsafe baseline
	 *  declaration). Distinct from Absent: this lever never entered the live registry at all, so
	 *  a caller asking about it by name gets nothing back -- see FPMLeverRegistry's refusal ledger
	 *  for WHY, rather than a silent absence indistinguishable from a name that was never tried. */
	Refused,
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMLeverAvailability Availability);

/**
 * ONE LEVER. Plain data -- no behaviour lives here, matching FPMCVarWriter::FHoldView's shape
 * (a flat struct the registry owns, not a class with its own logic).
 */
struct FICSITSPERFORMANCEMANAGER_API FFPMLeverDefinition
{
	/** Stable id. Registry key. */
	FName Name;

	EFPMLeverBacking Backing = EFPMLeverBacking::Cvar;

	/** For `Cvar` backing: the cvar(s) this lever moves, applied through FPMCVarWriter. A multi-
	 *  cvar lever (several r.* names moved together as one conceptual tunable) lists all of them;
	 *  every name is checked against Law 1 and clause 2 independently -- one bad name refuses the
	 *  whole registration rather than silently dropping just that member. */
	TArray<FString> CVarNames;

	/** For `ScalabilityGroup` backing: the CSS group name (e.g. "GlobalIlluminationQuality"). The
	 *  member cvars this actually writes come from FPMLeverRegistry::GetAliasMembers, not from
	 *  writing this name as a cvar -- see EFPMLeverBacking's comment. */
	FString GroupName;

	EFPMLeverPolicy Policy = EFPMLeverPolicy::Absolute;

	/** Declared spend axis/axes. See EFPMLeverCurrency's comment -- an axis declaration, not a
	 *  fabricated number. */
	EFPMLeverCurrency Currency = EFPMLeverCurrency::None;

	/** Free text, e.g. "part of Bonus +2 tier, ~0.8ms shared across 6 cvars (carried FPM1 table)".
	 *  Empty means "unmeasured -- the bench assigns this", stated rather than implied by a zero. */
	FString EstimatedCost;

	/** GPU-side levers are never on a dedicated server by default -- the individual registration
	 *  states its own answer rather than inheriting the registry container's Any (see
	 *  FPMLeverRegistry's own Side(), which is Any because the CONTAINER is side-agnostic). */
	EFPMFixSide Side = EFPMFixSide::NeverOnDedicatedServer;

	/** Required (enforced at registration) for MaxOf/MinOf/BaseScale/BaseDelta. See Law 3's
	 *  comment on EFPMLeverBaselineSource. */
	EFPMLeverBaselineSource BaselineSource = EFPMLeverBaselineSource::NotApplicable;

	/** Where the DEFINITION came from -- "carried FPM1 :205-236 byte-verified", "new, self-test
	 *  fixture", etc. Not the bench's per-measurement provenance enum -- see the file header. */
	FString Provenance;

	/** Free text tier/band catalogue label ("K3", "+2"). Drives no behaviour -- see the file
	 *  header's "tier/band" note. */
	FString TierHint;

	/** §3.12: a cheap local check that this lever's subsystem exists on this machine (upscaler
	 *  plugin present, cvar registered, group tier reachable, VRAM tier met, platform). Null means
	 *  "fall back to a plain cvar-exists check" (FPMLeverRegistry::ProbeOne) -- a lever that names
	 *  no probe and no cvar stays Unknown rather than being guessed Available. Run at OnWorldLoad
	 *  only, never per-tick, per §3.12's own rule. */
	TFunction<bool()> CapabilityProbe;

	// ---- Runtime state below. Filled by the registry; a caller constructing a definition for
	// ---- RegisterWritable/RegisterReadOnly should leave these at their defaults. ----

	/** True only if this definition reached the live registry through RegisterWritable and was not
	 *  refused. A RegisterReadOnly entry is always false here. */
	bool bWritable = false;

	EFPMLeverAvailability Availability = EFPMLeverAvailability::Unknown;

	/** Set once by FPMLeverRegistry::CaptureBaselineOnce; see its own comment for the guarantee. */
	bool bBaselineCaptured = false;
	FString CapturedBaselineValue;
};
