// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Session/FPMHostProbeSubsystem.h"

AFPMHostProbeSubsystem::AFPMHostProbeSubsystem()
{
	// SpawnOnServer_Replicate: USubsystemActorManager spawns this on the server side and sets
	// bReplicates true for it (SubsystemActorManager.cpp:20-32) - this is the ONLY policy value that
	// answers "does the host run FPM" from a client's point of view. SpawnOnServer (without
	// _Replicate) never leaves the machine that spawned it, and SpawnLocal / SpawnOnClient spawn a
	// copy everywhere regardless of what the host runs, which would make every client read FULL
	// unconditionally and defeat the entire probe.
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;
}
