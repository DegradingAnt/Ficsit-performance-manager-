// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "FPMHostProbeSubsystem.generated.h"

/**
 * ★ THE HOST PROBE — design §5.9's replicated presence signal, and NOTHING ELSE.
 *
 * WHAT THIS IS. An SML `AModSubsystem` (`Subsystem/ModSubsystem.h`), spawned with
 * `ESubsystemReplicationPolicy::SpawnOnServer_Replicate`: `USubsystemActorManager` spawns one instance
 * on whichever machine is the SERVER (dedicated, listen, or the local machine in singleplayer) and the
 * engine's OWN replication then delivers it to every connected client — the same mechanism that
 * replicates any other actor in this game (`SubsystemActorManager.cpp:20-32`). FPM opens no connection
 * of its own to make this happen.
 *
 * WHY EXISTENCE IS THE WHOLE SIGNAL. This class carries no replicated property and answers no RPC. The
 * fact FPM needs is binary — "does the host run FPM" — and that fact IS the actor's existence: a server
 * without FPM never compiles this class into its binary, so it can never register, spawn, or replicate
 * it. There is nothing to read FROM the actor, because reading anything beyond "did it arrive" would be
 * a cvar-shaped guess standing in front of a structural fact that already answers the question.
 * `FFPMHostTier` (`Session/FPMHostTier.h`) is what turns "arrived / did not arrive within budget" into
 * the FULL / NO-HOST-REPLICA tier and the player-facing line, and for WHY that third state is named
 * for the observation rather than for a verdict about the host (review 2026-08-15, HIGH 1: with
 * RequiredOnRemote true and an exact pin, a joined client cannot be on an FPM-less host, so an
 * absent replica indicts THIS probe, not the server). This class never prints, logs, or decides
 * anything itself.
 *
 * ⚠ NOT THE JOIN-VERSION CHANNEL. A refused join (a version mismatch under `RequiredOnRemote`) is a
 * SEPARATE, EARLIER decision SML itself makes before any world exists — before this actor could ever
 * spawn — and a different piece of Slice 4 work owns telling the player which side is on which version
 * when that refusal happens. This probe answers a later question: for a JOIN THAT SUCCEEDED, is the
 * host also running FPM. Keeping the two apart is deliberate scope discipline, not an oversight — a
 * version field here would duplicate work another change owns and give two places a mismatch could
 * disagree.
 *
 * WHICH SIDE OF THE NO-NETWORK LAW THIS SITS ON, STATED EXPLICITLY (hard rule 2 — no network activity
 * FPM opens itself, ever). This is ordinary `AActor` replication over the game's OWN already-open
 * connection — the identical mechanism vanilla uses to replicate any actor, and the identical mechanism
 * `ASessionSettingsSubsystem` / `AChatCommandSubsystem` already use elsewhere in SML. FPM opens no
 * socket, sends no HTTP request, and calls no API outside the engine's replication system. If this ever
 * grows an RPC that ASKS the host something, that crosses the line this comment draws; existence-only
 * detection does not.
 */
UCLASS()
class FICSITSPERFORMANCEMANAGER_API AFPMHostProbeSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	AFPMHostProbeSubsystem();
};
