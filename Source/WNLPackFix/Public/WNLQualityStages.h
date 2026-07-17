#pragma once

#include "CoreMinimal.h"

/**
 * WNL quality stages — the declarative lever tables the governor's stage engine walks.
 *
 * One QualityStage integer spans -4..+6. Stage 0 = the USER'S OWN SETTINGS (vanilla-or-better
 * guaranteed — the v0.8.4 law). Positive stages are cumulative headroom-gated bonuses up to
 * beyond-Cinematic; negative stages are the emergency cut ladder, engaged only at the resolution
 * floor while GPU-bound. Values and tier placement come from engine defaults
 * and our own boot measurements, validated one tier per boot on our own hardware.
 *
 * Policies express how a lever combines with the user's captured baseline so a stage can never
 * push a lever BELOW a better value the user already had (bonuses) and cuts revert exactly.
 */
enum class EWNLLeverPolicy : uint8
{
	Absolute,  // write the value as-is
	MaxOf,     // max(baseline, value): higher-is-better bonus / higher-is-worse cut, never past baseline
	MinOf,     // min(baseline, value): LOWER-is-better lever (e.g. fog GridPixelSize) never coarsens a sharper user baseline
	BaseDelta, // baseline + value (e.g. VSM bias sharpen -0.5, GI tier -1)
	BaseScale, // baseline * value (e.g. Lumen probe downsample x2)
};

struct FWNLStageLever
{
	const TCHAR*    CVar;
	float           Value;
	EWNLLeverPolicy Policy   = EWNLLeverPolicy::Absolute;
	bool            bFloat   = false;   // write as float (default int)
	int32           MinVramMB = 0;      // skip lever below this dedicated VRAM (0 = no gate)
	float           ClampMin = -1.e9f;  // applied after the policy math
	float           ClampMax = 1.e9f;
};

struct FWNLQualityStage
{
	int32        Stage;      // +1..+6 or -1..-4
	const TCHAR* Name;
	float        EstCostMs;  // engage-cost estimate; replaced by measured LearnedCostMs at runtime
	TArray<FWNLStageLever> Levers;
};

/** Bonus stages +1..+6, ascending. Cumulative: stage N applies tables 1..N (later overrides earlier). */
const TArray<FWNLQualityStage>& WNLGetBonusStages();

/** Cut stages -1..-4 (index 0 = stage -1). Cumulative: stage -N applies tables -1..-N. */
const TArray<FWNLQualityStage>& WNLGetCutStages();
