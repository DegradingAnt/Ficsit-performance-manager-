// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * SERVER LEVER AUDIT - which of the dedicated-server governor's candidate levers EXIST on this
 * build, and which of those will accept a write.
 *
 * ★ IT ANSWERS THE DESIGN'S OWN NAMED BLOCKER, WORD FOR WORD. The server governor's scope is ruled
 * (`FPM2-DESIGN-ASSEMBLED.md:1369` - "ONLY levers a server can move: sim/net/GC/save cadence,
 * NetworkQuality, Cartograph budgets, FG.Lightweight budgets; never renderer"), but the research
 * behind it stops at the same sentence four separate times:
 *   L1 save cadence  - "Runtime-settable: **unverified**. Defaults **unmeasured**."
 *   L3 FG.Lightweight - "Runtime-settable: **unverified for every one of them.** ... no probe has
 *                        been run."
 *   L5 net.*          - "Runtime-settable / defaults / sizes: **all unmeasured.**"
 *   (`RESEARCH-R41-SERVER-SIDE-2026-08-08.md:234-317`)
 * Nothing in the governor can be designed on top of a lever list whose reachability is unknown, and
 * the whole list was assembled from a cvar dump taken on a CLIENT. A dedicated server is a different
 * build with different modules compiled in, so "the name exists" is a claim about the wrong binary.
 * One server boot with this armed replaces every "unverified" above with a fact.
 *
 * ⚠ WHAT THIS IS NOT. It is not a governor and it is not a lever. It writes NOTHING - no cvar, no
 * ini, no save, no network. There is no `ECVF_SetByCode` write here and no `FPMCVarWriter` call,
 * because a reader that can also write is a reader nobody can trust on a live server.
 *
 * ★ THE LIVENESS QUESTION, ASKED AND ANSWERED IN THE CODE. "What concrete input makes this report a
 * non-zero or a failure?" - a lever name that is absent from the server build, or present and
 * `ECVF_ReadOnly`. Both are expected: the list was harvested on a client, so a non-trivial number of
 * ABSENT verdicts is the predicted result rather than a fault. And because "everything is fine" and
 * "the lookup is broken" would otherwise print identically, the classifier proves itself against a
 * known-positive and a known-negative before it prints anything at all
 * (`FPMServerLeverClassifierIsAlive`, mirroring `FPMSaveSettingsInterceptor.cpp:63`). A failed
 * self-test REFUSES the report instead of publishing zeros.
 *
 * ⚠ AND IT PRINTS ITS OWN COVERAGE. The table is a fixed list of NAMED candidates, so it can say
 * nothing about a lever nobody wrote down. Every report ends with how many names it asked about,
 * which is the denominator a reader needs before treating "12 settable" as good news.
 *
 * WHEN IT RUNS: once, on the first world load, and only where `IsRunningDedicatedServer()`. Not at
 * `Arm()` - a console variable owned by a module that has not finished loading does not exist yet,
 * and an audit run too early reports ABSENT for levers that are merely late. World load is the first
 * moment every mod's game feature has activated.
 */
class FFPMServerLevers final : public IFPMFix
{
public:
	static FFPMServerLevers& Get();

	virtual const TCHAR* Name() const override { return TEXT("server-levers"); }

	/**
	 * `Any`, and it self-gates. `EFPMFixSide` has exactly two values (FPMFixContract.h:50) and neither
	 * is "dedicated server only", so a server-only unit declares `Any` and refuses inside `Arm()` -
	 * the same shape `Fixes/Interop/FPMWwiseServerGate.h:38` already documents. The refusal is LOGGED,
	 * so a client log says why this is quiet rather than leaving the reader to guess.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * `UnknownCause`, the same answer and the same reason as the GC meter. Nothing is being fixed
	 * here; the entire value is producing a number the design does not have, and claiming any stronger
	 * status before the boot is the drift the enum exists to stop.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::ServerLevers; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;
	virtual void Disarm() override;

	/** `FPM.Server.Levers` - re-run the audit and print it to whoever asked. */
	static void ReportNow(FOutputDevice& Ar);
};
