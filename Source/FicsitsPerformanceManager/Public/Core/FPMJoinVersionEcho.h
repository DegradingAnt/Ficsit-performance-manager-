// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

#include "Core/FPMFixContract.h"

/**
 * JOIN VERSION ECHO — makes a version-mismatch join refusal LEGIBLE. Ant's ruling, verbatim in effect:
 * the version pin STAYS EXACT, and the refusal must say WHICH SIDE IS ON WHICH VERSION, diagnosable from
 * what the player sees, without opening a file.
 *
 * ★ WHAT THIS IS NOT. It does not touch SML's join decision. `RemoteVersionRange = "=0.11.26"` in the
 * .uplugin keeps refusing an exact mismatch exactly as before — that pin is Ant's, not ours to loosen.
 * This is a second, INDEPENDENT observer of the same handshake, built because SML's own refusal text
 * cannot be reached or edited from mod code (see below), and because — traced in
 * SMLNetworkManager.cpp:165-176 — SML's own message names NEITHER side's version when the remote never
 * reports the mod at all, not even ours, which we do know.
 *
 * ★ THE HOOK POINT AND WHY. `FSMLNetworkManager::HandleMessageReceived(UNetConnection*, FString Data)`
 * (SMLNetworkManager.h:16, public, SML_API) fires on BOTH client and server the moment either side's mod
 * list arrives (SMLNetworkManager.cpp:31-32, `bServerHandled = bClientHandled = true`) — well before
 * `ValidateSMLConnectionData` can close the connection, which happens later, "after key exchange"
 * (SMLNetworkManager.h:21). An AFTER hook here reads `(Connection, Data)`: `Data` is the SAME JSON mod
 * list SML itself is about to parse, and `Connection` resolves to the live `UGameInstance` via
 * `UModNetworkHandler::GetGameInstanceFromNetDriver` (NetworkHandler.h:99) — the identical helper SML's
 * own `ValidateSMLConnectionData` uses, which is also correct while still mid-connect
 * (`GetWorldContextFromPendingNetGameNetDriver`, NetworkHandler.cpp:100).
 *
 * ★ WHAT IS NOT REACHABLE, WITH THE RECEIPT. `ValidateSMLConnectionData` (SMLNetworkManager.cpp:143-194)
 * builds its `RemoteMissingMods` array and `Reason` string as LOCAL VARIABLES and hands them straight to
 * `CloseWithFailureMessage` — neither is a parameter, a return value, or reachable through any public
 * accessor, and `GModConnectionMetadata` (:22) is a file-static annotation with none either. A hook on
 * `ValidateSMLConnectionData` itself sees only `(Connection, IsServer)` — no version data. So this fix
 * cannot edit or improve SML's own on-the-wire failure text; it can only stand next to it with an
 * independently-sourced, honestly-scoped statement of the same facts, timed to land first.
 *
 * ★ THE LINE THIS SITS ON. Hard rule 1 bans a transport FPM opens itself; it explicitly does not cover
 * the game's own replication and join handshake. This fix opens no socket and sends nothing extra over
 * the wire — it reads data the handshake already delivered to a hooked function's parameters, the exact
 * carve-out SMLNetworkManager.cpp itself lives inside.
 *
 * ★ HOW IT REACHES THE SCREEN. FPM cannot write to FactoryGame's own connect-failure Blueprint UI. What
 * it CAN do is `FPMOverlay` (FPMOverlay.h) — raw Slate attached to `GEngine->GameViewport`, which is
 * viewport-scoped rather than world-scoped BY DESIGN (FPMOverlay.h:16-20) precisely so it survives a
 * moment a UMG panel cannot, including a disconnect-back-to-menu transition. On a mismatch or an absent
 * remote, this fix posts to it and forces it visible — it must not depend on the overlay's current
 * default visibility, which is stated to be temporary ("on by default while the mod is pre-release",
 * FicsitsPerformanceManager.cpp:375).
 *
 * ★ THE HONESTY RULE, Ant's Rule 4. If the remote never reports FicsitsPerformanceManager at all, its
 * version is NOT available here — full stop. The message says exactly that; it never infers, guesses, or
 * states a version for a side that did not send one. A confident wrong message is worse than none.
 *
 * ★ THE SELF-TEST. `Arm()` happens in `StartupModule`, before any world or `UGameInstance` exists
 * (FPMHookLedger.h:37), and the classifier needs `UModLoadingLibrary`, a `UGameInstanceSubsystem` — so
 * the dead-instrument proof cannot run at arm time and runs once from `OnWorldLoad` instead, the
 * contract's own designated "a GameInstance now exists" moment. It feeds the SAME `FVersionRange::Matches`
 * SML itself calls a known-MATCH probe (our own live version), a known-MISMATCH probe (that version with
 * the patch bumped by one) and a known-ABSENT probe (no entry at all), and asserts all three classify
 * correctly — mirroring FPMSaveSettingsInterceptor.cpp's known-positive/known-negative pattern. If it
 * ever fails, the runtime handler keeps logging the raw facts (never suppressed) but stops asserting a
 * match/mismatch verdict it cannot back up.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMJoinVersionEcho final : public IFPMFix
{
public:
	static FFPMJoinVersionEcho& Get();

	virtual const TCHAR* Name() const override { return TEXT("join-version-echo"); }

	/**
	 * Any. A dedicated server has no viewport and therefore no FPMOverlay, but it has FactoryGame.log,
	 * and the operator reading it (Ant, most nights) needs the same both-sides answer a client player
	 * would see on screen — arguably more, since nobody else is watching a screen on that machine at all.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * Guard: the harm is an undiagnosable refusal, and the cause — SML's own message construction, terse
	 * in the absent-remote branch and unreachable from mod code either way — is not ours to own or edit.
	 * This prevents the harm; it does not repair the cause.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::JoinVersion; }

	virtual void Arm() override;

	/** Removes the HandleMessageReceived subscription. Without this, `FPM.Fix.JoinVersionEcho 0` and
	 *  `FPM.Enabled 0` would report the fix disarmed while it kept observing every join. */
	virtual void Disarm() override;

	/** Runs the classifier self-test exactly once, the first time a GameInstance is known to exist. */
	virtual void OnWorldLoad(UWorld* World) override;

	/** True once the self-test has run and passed. The runtime handler reads this before asserting a
	 *  match/mismatch verdict — fail closed, same shape as `FFPMSaveSettingsInterceptor::IsHealthy()`. */
	static bool IsClassifierProven();

private:
	/** The real work, called from the AFTER hook once per handshake message, either side. Named rather
	 *  than left as the hook lambda's body so the lambda passed to FPM_SUBSCRIBE_AFTER stays a one-liner
	 *  — FPMHookLedger.h's own warning about a top-level comma splitting the macro's arguments. */
	void HandleHandshakeMessage(class UNetConnection* Connection, const FString& Data);

	/** The HandleMessageReceived subscription. Invalid in the editor, where the ledger refuses the install. */
	FDelegateHandle JoinHandle;

	bool bSelfTestRun = false;
	bool bClassifierProven = false;
};
