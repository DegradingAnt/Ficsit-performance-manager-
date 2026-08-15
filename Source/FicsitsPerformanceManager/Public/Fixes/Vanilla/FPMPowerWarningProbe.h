// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * POWER-WARNING PROBE — answers whether the "Fuse Blown" popup is lying. It changes nothing.
 *
 * THE SYMPTOM (board m5663571): every login reports the power network is down when it is not. The
 * popup is titled "Fuse Blown" and Ant sees it in single-player AND on the server.
 *
 * ★ WHY THIS READS STATE INSTEAD OF HOOKING THE EMITTER, WHICH IS THE OBVIOUS MOVE AND THE WRONG ONE.
 *
 * The warning comes from `AFGCircuitSubsystem::PowerCircuit_OnFuseSet` (`FGCircuitSubsystem.h:143`).
 * That is a **`BlueprintNativeEvent`**, and its native `_Implementation` is an empty body
 * (`FGCircuitSubsystem.cpp:47`) because `BP_CircuitSubsystem` implements the popup in Blueprint. So a
 * `SUBSCRIBE_METHOD` on the native side would most likely never fire, and cancelling it would not stop
 * the Blueprint graph either. That hook would compile, arm, print nothing, and read exactly like "the
 * bug never happened" — the single most expensive failure shape this project has, and the reason the
 * schematic probe exists at all.
 *
 * So the question is asked the other way round: **at login, is ANY circuit actually fuse-triggered?**
 * `UFGPowerCircuit::IsFuseTriggered()` is public and inline (`FGPowerCircuit.h:163`), reading the
 * replicated `mIsFuseTriggered`. If the popup fires while every circuit reports false, the warning is
 * lying and the origin is named. If one reports true, the warning is TELLING THE TRUTH about a derelict
 * over-capacity circuit, and suppressing it would have hidden a real fault.
 *
 * ⚠ THAT SECOND OUTCOME IS THE ONE TO PLAN FOR. Two research passes found the vanilla graph never reads
 * the `circuits` payload it is handed, so an empty array still pops the warning — which makes a false
 * positive plausible. Plausible is not measured. Nothing is suppressed until this probe says so.
 *
 * ★ IT READS BOTH CONTAINERS, AND THAT IS NOT REDUNDANCY.
 * `mCircuits` is a `TMap` (`:251`) and **a TMap does not replicate** — the header says so itself at
 * `:257`, which is why `mReplicatedCircuits` (`:259`) exists as a parallel array. On a joining client
 * the map can be EMPTY while the array is populated. A probe that read only the map would report "no
 * circuits" on exactly the machine where the popup is being complained about, and that zero would look
 * like an answer.
 *
 * ★ AND IT SAMPLES OVER TIME RATHER THAN ONCE. `mIsFuseTriggered` is `ReplicatedUsing`
 * (`FGPowerCircuit.h:354`), so a client's copy arrives after the join, not during it. One reading taken
 * at world load would measure the replication delay and call it the answer.
 * [[snapshot-is-not-a-measurement]] — every wrong number this project has produced came from a single
 * instant read.
 *
 * VIEWER ONLY. It installs no hook, writes no console variable, touches no circuit, and never calls
 * `ResetFuse`. `Debug_DumpCircuitsToLog` is deliberately NOT used: it dumps everything to the log, and
 * Ant asked for the relevant lines, not the whole log.
 */
class FFPMPowerWarningProbe final : public IFPMFix
{
public:
	static FFPMPowerWarningProbe& Get();

	virtual const TCHAR* Name() const override { return TEXT("power-warning-probe"); }

	/**
	 * Both sides, and the asymmetry is the point. The server owns the authoritative fuse state; the
	 * client is where the popup appears and where the replicated copy may disagree or lag. Running it on
	 * one side only could not tell a real trip apart from a replication artefact.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** The design's own classification for this item: origin-naming diagnostic FIRST, then a fix if one
	 *  is warranted. Nothing here is claimed to be repaired. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::PowerWarning; }

	virtual void Arm() override;

	/** Starts the post-load sampling window. */
	virtual void OnWorldLoad(UWorld* World) override;

	/** Stops the sampler. */
	virtual void Disarm() override;

	/** `FPM.Power.Report` — sample once, now, and print the verdict. */
	static void ReportNow();

	/**
	 * `FPM.Power.Chat` — the CHAT-FACING verb (board m5663571 §7.2). `ReportNow()` above answers "is the
	 * popup lying" for the log; this answers the same question for the player, in the surface she
	 * actually reads. Ant's own rule, stated of a different command: "print stuff to the chat instead...
	 * print what is relevant, not the entire log" — so this prints ONLY the tripped circuits, never
	 * `Debug_DumpCircuitsToLog`'s everything.
	 *
	 * ⚠ ALWAYS SAYS SOMETHING, even when nothing is tripped — a command that prints nothing on a healthy
	 * grid is indistinguishable from a broken command.
	 * ⚠ ALWAYS PRINTS COVERAGE — circuits examined, tripped, and unreadable — so a bare "none tripped"
	 * can be told apart from a probe that examined nothing.
	 * Client-only by construction (`FPMChat` no-ops on a dedicated server); the console command below
	 * reports that explicitly rather than going silent.
	 */
	static void ReportTrippedToChat();

	/**
	 * Same private-container walk as `ReadCircuitCounts`, extended to name WHICH circuits are tripped.
	 * A second member function rather than another out-param on `ReadCircuitCounts`, because that one is
	 * already called every 2s by the login-window sampler and growing its signature for a chat-only need
	 * would make every existing call site carry an unused `TArray`.
	 *
	 * @param OutExamined   power circuits successfully read (cast to `UFGPowerCircuit` succeeded).
	 * @param OutUnreadable container slots that were null, or a `UFGCircuit` that was NOT a
	 *                      `UFGPowerCircuit` — the latter should not occur today (grep confirms
	 *                      `UFGPowerCircuit` is the only `UFGCircuit` subclass in this codebase), so it
	 *                      is counted rather than assumed away.
	 * @param OutTrippedCircuitIDs `GetCircuitID()` (`FGCircuit.h:57`, public, no AT needed) for every
	 *                      circuit whose `IsFuseTriggered()` (`FGPowerCircuit.h:163`, public) is true.
	 * @return false when the subsystem does not exist yet — NOT the same as "nothing tripped".
	 */
	static bool EnumerateTrippedCircuits(UWorld* World, int32& OutExamined, int32& OutUnreadable,
	                                     TArray<int32>& OutTrippedCircuitIDs);

	/**
	 * ★ THE ONLY PLACE THAT TOUCHES `AFGCircuitSubsystem`'s PRIVATE CONTAINERS, AND IT HAS TO BE A
	 * MEMBER OF THIS CLASS.
	 *
	 * `AccessTransformers.ini` grants friendship to a CLASS. A free function in an anonymous namespace
	 * is not a member of anything, so it inherits no access — the first version of this file put the
	 * read in one and the compiler was right to refuse it:
	 *   `error C2248: 'AFGCircuitSubsystem::mCircuits': cannot access private member`
	 * The sibling fixes work because their reads sit inside lambdas defined in member functions, and a
	 * lambda carries its enclosing class's access. Stated here because the mistake is invisible until
	 * the build fails, and the failure names the field rather than the reason.
	 *
	 * Counts only, by design — no circuit pointer escapes this call, so no caller can be tempted to
	 * hold one past the tick. The header warns that a circuit pointer is valid only for the current
	 * tick (`FGCircuitSubsystem.h:81`).
	 *
	 * @return false when the subsystem does not exist yet, which is NOT the same as "nothing tripped".
	 */
	static bool ReadCircuitCounts(UWorld* World, int32& OutMapCircuits, int32& OutReplicatedCircuits,
	                              int32& OutPowerCircuits, int32& OutFuseTriggered);
};
