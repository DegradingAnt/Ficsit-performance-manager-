// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMGiveTake.h"
#include "Core/FPMStageApply.h"
#include "Core/FPMSteerSignal.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

/**
 * ★ ONE GOVERNOR STEP, DRIVEN BY HAND. Signal -> decision -> apply -> commit, once, on demand.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★ WHY THIS IS A COMMAND AND NOT A TIMER
 *
 * The four pieces of the governor are built and wired: FFPMSteerSignal produces the inputs,
 * FFPMGiveTakeWalk decides, FFPMStageApply writes, and Commit moves the ladder. What does not exist
 * is the reason to run them automatically, and the design says so in three separate places:
 *
 *   - No BENCH (section 4), so no tier has a measured cost, and section 3.5a excludes every one of
 *     them: "never worse than vanilla -- unproven means CUT". In modes A/B/C the walk answers
 *     NoBenchProfile; in Balanced it answers ModeCarriesNoStageLevers, because Balanced steers
 *     resolution and CPU relief only.
 *   - No BIND ATTRIBUTION (section 3.6's middle tier), so every cut is refused as AttributionUnknown.
 *   - No RESOLUTION EXECUTOR (section 8), so a ResolutionDown decision has nothing to carry it out.
 *
 * A timer wired today would therefore run forever and never act. That is the textbook dead
 * instrument: a confident readout whose output cannot change, certifying the very thing it was built
 * to catch. A COMMAND is not, because a person runs it when they want an answer and the answer moves
 * with the live frame mean.
 *
 * ⚠ WHAT WOULD MAKE THE AUTOMATIC DRIVER WORTH BUILDING, stated so the next builder does not have to
 * guess: a bench profile that fills FFPMTierMeasurement, plus an attribution source. With those two,
 * the loop is this function on a timer and nothing else changes.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★ IT DOES NOT SAVE AND RESTORE THE WALK'S STATE, unlike FPM.Stage.Simulate
 *
 * FPM.Stage.Simulate asks a hypothetical question and puts the dwell and stall bookkeeping back
 * exactly as it found it. This is the opposite: a real step. The dwell timers must carry between
 * calls or the ladder could never move, since the give dwell is two seconds and a person cannot type
 * faster than that anyway -- which makes typing the command twice behave like the real driver at a
 * human cadence.
 *
 * ★ COMMIT ONLY IF THE APPLY CHANGED SOMETHING. FFPMGiveTakeWalk::Decide deliberately does not move
 * the ladder position; this is the caller that decides whether it moved. An apply that was refused
 * (resolution) or that found every lever unavailable must NOT leave the walk believing a tier is
 * engaged -- that belief is what would make the next decision skip a rung that never moved.
 */

namespace
{
	EFPMGovernorMode ParseGovernorMode(const FString& Text)
	{
		const FString Lower = Text.ToLower();
		if (Lower == TEXT("a") || Lower == TEXT("resolution")) { return EFPMGovernorMode::ResolutionFirst; }
		if (Lower == TEXT("b") || Lower == TEXT("graphics"))   { return EFPMGovernorMode::GraphicsFirst; }
		if (Lower == TEXT("c") || Lower == TEXT("lighting"))   { return EFPMGovernorMode::LightingFirst; }
		return EFPMGovernorMode::Balanced;
	}

	void GovernorStepOnce(const TArray<FString>& Args, FOutputDevice& Ar)
	{
		FPMReportGate Gate(Ar, TEXT("FPM.Governor.Step"));
		if (Gate.IsRefused()) { return; }
		FPMScopedConsoleEcho Echo(&Ar);

		// ⚠ MODE IS AN ARGUMENT, NOT A SETTING, and that is deliberate. Mode SELECTION is section 6's
		// surface with its own config keys and its own "modes are locked until a bench exists" rule
		// (R17). Accepting one here lets a step be taken in any mode for diagnosis without inventing
		// half of that surface, and without a stored value that the real selector would later fight.
		const EFPMGovernorMode Mode = Args.Num() > 0 ? ParseGovernorMode(Args[0])
		                                             : EFPMGovernorMode::Balanced;

		FFPMSteeringInputs In;
		FString Coverage;
		const bool bSteerable = FFPMSteerSignal::Get().BuildInputs(Mode, In, Coverage);

		Ar.Logf(TEXT("[FPM] governor step, mode %s."), LexToString(Mode));
		Ar.Logf(TEXT("[FPM]   SIGNAL: %s"), *Coverage);

		if (!bSteerable)
		{
			// Refusing to decide on an unprimed mean is not caution for its own sake: the mean starts
			// negative, and a decision taken against a placeholder would be a real decision taken
			// against a number nobody measured.
			Ar.Logf(TEXT("[FPM]   STOPPED: the signal is not primed yet, so no decision was taken. "
			             "Run this again after a few seconds of gameplay."));
			return;
		}

		const FFPMSteerDecision Decision = FFPMGiveTakeWalk::Get().Decide(In);
		Ar.Logf(TEXT("[FPM]   DECISION: action '%s' tier '%s' block '%s'%s"),
			LexToString(Decision.Action), LexToString(Decision.Tier), LexToString(Decision.Block),
			Decision.bResolutionSlewConcurrent ? TEXT(" (+ concurrent resolution slew)") : TEXT(""));
		Ar.Logf(TEXT("[FPM]   WHY: %s"), *Decision.Reason);

		if (!Decision.IsAction())
		{
			// A named block is the correct outcome today, and saying which one is the whole value of
			// this command: it is the difference between "the governor did nothing" and "the governor
			// is waiting on the bench".
			Ar.Logf(TEXT("[FPM]   NOTHING APPLIED: the walk returned a named block, not an action. "
			             "That is expected today -- there is no bench profile, so section 3.5a "
			             "excludes every stage tier, and there is no bind attribution, so every cut "
			             "is refused. Neither is a defect in this chain."));
			return;
		}

		const FFPMApplyResult Result = FFPMStageApply::Get().Execute(Decision);
		FFPMStageApply::PrintResult(Result, Ar);

		if (Result.ChangedAnything())
		{
			FFPMGiveTakeWalk::Get().Commit(Decision, In.NowSeconds);
			Ar.Logf(TEXT("[FPM]   COMMITTED: the ladder moved and tier %s started its cooldown."),
				LexToString(Decision.Tier));
		}
		else
		{
			Ar.Logf(TEXT("[FPM]   NOT COMMITTED: the apply changed nothing, so the ladder position is "
			             "unchanged. This split is the point -- a refused or empty apply must never "
			             "leave the walk believing a tier is engaged."));
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GFPMGovernorStepCmd(
	TEXT("FPM.Governor.Step"),
	TEXT("Take ONE real governor step: read the live signal, decide, apply, commit. "
	     "Usage: FPM.Governor.Step [balanced|A|B|C]. This WRITES if the decision is an action."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&GovernorStepOnce));
