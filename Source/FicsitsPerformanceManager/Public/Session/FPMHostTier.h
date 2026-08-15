// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Core/FPMFixContract.h"

/**
 * WHAT THE PLAYER IS TOLD — design §5.9's three states, one of which is deliberately "do not know yet"
 * rather than a guess in either direction (the dead-instrument rule: a field that cannot determine
 * something must say so, not omit it).
 */
enum class EFPMHostTier : uint8
{
	/** Client only. From world-load until the host's replica arrives or the budget ends. Nothing about
	 *  the host's fixes is claimed while this holds - it is the honest "do not know yet" state. */
	Probing,

	/** This machine IS the host (standalone / listen / dedicated), or a client has SEEN the host's
	 *  replica via the game's own replication. KNOWN, never inferred from a cvar or a guess. */
	Full,

	/**
	 * Client only: the budget elapsed and NO host replica arrived. This is the OBSERVATION, not a
	 * verdict about the host. It was called `Vanilla` until the 2026-08-15 review (HIGH 1) showed that
	 * "the host has no FPM" is an input this state cannot receive on a shipped client, so the name
	 * asserted something the wiring could never carry. See the ★ REACHABILITY block below.
	 * Can still upgrade to Full if the replica arrives late.
	 */
	NoHostReplica,
};

/** For log lines and the tier line. */
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMHostTier Tier);

/**
 * ★ THE PURE CLASSIFIER — kept apart from the ticking/world/actor plumbing on purpose, so the one
 * decision that matters can be proven with plain values instead of a live replicated actor and a real
 * remote host, neither of which exists in a solo boot. `FFPMHostTier::SelfTest` drives this with a
 * known-positive and a known-negative for every branch, the same discipline `FPMSaveSettingsInterceptor
 * .cpp:63` and `FPMWristSlotComponent.cpp:334` already use for a decision this project cannot safely
 * rehearse for real without a second machine.
 *
 * PRESENCE ALWAYS WINS, at any elapsed time — that single rule is what makes late-arrival upgrade safe
 * without a separate code path: a replica seen at second 45 classifies FULL exactly like one seen at
 * second 2. The CALLER is what refuses to let a later ABSENCE downgrade an already-latched FULL — this
 * function has no memory of a previous call and cannot do that job; see `FFPMHostTier::PollTick`.
 *
 * ★ THE MIRROR CASE THIS FUNCTION EXISTS TO REFUSE: a correct FULL host whose replica simply has not
 * replicated YET (ordinary join latency) must classify PROBING, never `NoHostReplica`, while still
 * inside the budget — calling that host's replica ABSENT would tell the player crash guards are off
 * when they are not. That is `SelfTest`'s known-negative case.
 */
FICSITSPERFORMANCEMANAGER_API EFPMHostTier FPMClassifyHostTier(
	bool bSelfIsHost, bool bReplicaObserved, double ElapsedSeconds, double TimeoutSeconds);

/**
 * ★ THE HOST PROBE'S CLIENT-SIDE STATE MACHINE AND THE TIER LINE — design §5.9, Slice 4.
 *
 * WHAT RUNS WHERE. `OnWorldLoad` reads `World->GetNetMode()` once per world load (never per frame —
 * authority does not change mid-session) and splits in two:
 *
 *   - NOT a client (standalone / listen / dedicated): this machine IS the host. Tier is `Full`
 *     immediately, KNOWN rather than probed — there is no second machine to wait on, the
 *     server-authoritative fixes above this one in the arm order are already running right here. This
 *     branch ALSO runs the local-authority self-test below.
 *   - A client: starts a 1 Hz poll (`FTSTicker`, matching `FPMOverlay`'s own ticking pattern) that
 *     checks whether `AFPMHostProbeSubsystem` has replicated in yet. Present -> `Full`. Absent past the
 *     30s budget -> `NoHostReplica`. The poll keeps running afterwards, unbounded, so a late-arriving
 *     replica still upgrades the session live, and so a replica that later vanishes is caught as a WARNING
 *     rather than silently believed.
 *
 * ★ THE LOCAL-AUTHORITY SELF-TEST, THE ANSWER TO "WHAT PROVES THE WIRING WORKS WITHOUT A SECOND
 * MACHINE". `RootGameWorld_FicsitsPerformanceManager`'s `DispatchLifecycleEvent` calls
 * `Super::DispatchLifecycleEvent` — which is what registers `AFPMHostProbeSubsystem` into
 * `USubsystemActorManager` and spawns it SYNCHRONOUSLY on an authoritative world
 * (`SubsystemActorManager.cpp:11-43`) — BEFORE `FPMFixes::NotifyWorldLoad` runs. So by the time this
 * fix's `OnWorldLoad` executes on any authoritative world, its OWN probe actor must already be locally
 * discoverable. If it is not, that is FPM's OWN registration breaking, not "the host lacks FPM" — this
 * machine indisputably runs FPM, it is executing this line — and it is logged as an Error rather than
 * folded into the ordinary tier line. This check is exercised on EVERY authoritative boot (every
 * singleplayer session, every dedicated-server boot, every listen host), which is what makes it a real
 * known-positive rather than a synthetic one.
 *
 * ★ REACHABILITY, AND WHY THE THIRD TIER IS NOT "THE HOST IS VANILLA". Review finding 2026-08-15,
 * HIGH 1. It is the dead-instrument mirror: a state reachable ONLY through this instrument's own
 * failure, printed as a fact about the world.
 *
 * `FicsitsPerformanceManager.uplugin:56` sets `"RequiredOnRemote": true` and `:55` pins
 * `"RemoteVersionRange": "=0.11.26"`. Ant has ruled that BOTH stay; the descriptor is correct and it
 * was the readout that was wrong. SML enforces that pin on BOTH ends, not just the server:
 * `HandleWelcomePlayer` -> `ValidateSMLConnectionData(Connection, true)` and
 * `HandleWelcomePlayer_Client` -> `ValidateSMLConnectionData(Connection, false)`
 * (`SMLNetworkManager.cpp:58-64`). Inside it, any local mod the remote never reported is added to
 * `RemoteMissingMods` unless `!bRequiredOnRemote`, with an exemption for SML's own name on the client
 * side only (`:66-77`), and a non-empty list closes the connection (`:89-94`). A client that is
 * reading this tier line has therefore ALREADY completed a join, which no host lacking FPM at the
 * pinned version could have allowed.
 *
 * The single documented escape is SML's own `SML.SkipRemoteModListCheck` (`SMLNetworkManager.cpp:15-20`,
 * default off outside the editor), which disables that gate on the machine it is set on.
 *
 * So `ComposeTierLine` READS that cvar instead of assuming, and says which of the two worlds it is in:
 * gate on means an absent replica is FPM's OWN registration / spawn / replication failing and must be
 * reported as an FPM bug; gate off means a genuinely FPM-less host is reachable and this probe cannot
 * separate the two causes, which it states rather than guessing. `FPMHostProbeSubsystem.h` calls the
 * join channel "a SEPARATE, EARLIER decision" and that is still true: the join echo makes a refusal
 * legible, this probe answers a later question for a join that SUCCEEDED. What was missing was the
 * consequence of the earlier decision on this one, which is this block.
 *
 * ★ WHAT REMAINS UNPROVEN WITHOUT A SECOND MACHINE, SAID OUT LOUD. The client-side 30s timeout, the
 * late-arrival upgrade, and the mid-session-vanish warning all need an actual remote host to exercise
 * for real. Per the block above, the only host that can produce the timeout on a shipped client is one
 * whose own FPM is present but not replicating, or a client running `SML.SkipRemoteModListCheck 1`.
 * `SelfTest` proves the CLASSIFIER those branches are built on, and the local-authority check proves
 * the WIRING they share; neither is a substitute for an execution-proven two-machine join, and
 * `FPM.Status` says so.
 *
 * DOWNGRADE REFUSAL. Design §5.9: "a vanishing subsystem mid-session is a fault to surface, not a tier
 * change." `PollTick` enforces this directly — once `Tier` reads `Full`, only a WARNING is emitted for
 * a later absence; `Tier` itself never regresses.
 *
 * WHICH SIDE OF THE NO-NETWORK LAW THIS SITS ON. Every fact this class reports comes from either
 * `World->GetNetMode()` (local engine state) or `USubsystemActorManager::GetSubsystemActor` (a local
 * read of already-replicated actor state). Nothing here opens a connection, sends a request, or awaits
 * a response — see `FPMHostProbeSubsystem.h` for the same statement about the actor itself.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMHostTier final : public IFPMFix
{
public:
	static FFPMHostTier& Get();

	virtual const TCHAR* Name() const override { return TEXT("host-tier"); }

	/**
	 * `Any`. `EFPMFixSide` has no "client-only" value (`FPMFixContract.h:50-57`), and this fix
	 * genuinely has authoritative-side work: on an authoritative world it runs the local wiring
	 * self-test and declares `Full` immediately, so `Any` is the correct answer rather than a
	 * self-gated exception like `FFPMServerLevers`.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * `UnknownCause` — nothing is being repaired here. The tier line reports a FACT about the session,
	 * not a bug this fix owns fixing; same answer `FFPMServerLevers` gives, and for the same reason.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::HostTier; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;
	virtual void Disarm() override;

	/** Current tier for this world. `Probing` before the first `OnWorldLoad` of a session. */
	static EFPMHostTier CurrentTier();

	/** `FPM.Status` — the tier line, what it does and does not know, printed to whoever asked. */
	static void ReportStatus(FOutputDevice& Ar);

	/**
	 * `FPM.HostProbe.SelfTest` — drives `FPMClassifyHostTier` against a known-positive and a
	 * known-negative for every branch, and reports whether THIS SESSION's local-authority wiring
	 * check has run and passed. Returns false when any case disagrees; a caller must not trust the
	 * tier line on a failed self-test.
	 */
	static bool SelfTest(FOutputDevice* Ar);

private:
	bool PollTick(float DeltaSeconds);
	FString ComposeTierLine() const;
	void ReportTierLine() const;

	EFPMHostTier Tier = EFPMHostTier::Probing;
	TWeakObjectPtr<class UWorld> ProbedWorld;
	double ProbeStartWorldSeconds = 0.0;
	bool bVanishWarned = false;

	/** Coverage for `SelfTest` and `FPM.Status` to print — see `OnWorldLoad`. */
	bool bLocalAuthorityCheckRan = false;
	bool bLocalAuthorityCheckPassed = false;

	FTSTicker::FDelegateHandle PollHandle;

	static constexpr double TimeoutSeconds = 30.0;
	static constexpr float PollIntervalSeconds = 1.0f;
};
