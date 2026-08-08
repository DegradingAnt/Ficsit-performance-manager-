// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * WHERE A FIX IS ALLOWED TO ARM.
 *
 * This is NOT an authority enum, and the difference is the whole point. Arming happens in
 * StartupModule, before any world exists, so "does this machine have authority" is not a question that
 * can be asked yet — a listen host is a client build that happens to be the server. The only thing
 * decidable at arm time is whether this is a dedicated-server build, which is what gates renderer,
 * audio and input work.
 *
 * AUTHORITY IS A PER-CALL QUESTION AND STAYS INSIDE THE HANDLER: HasAuthority() at the call site, never
 * inferred from GetPlayerController(0).
 */
enum class EFPMFixSide : uint8
{
	/** Arms everywhere, SERVER INCLUDED. The handler self-guards on authority if it needs to. */
	Any,

	/** Renderer / audio / input work. Skipped on a dedicated server, which has none of them. */
	NeverOnDedicatedServer,
};

/*
 * ⚠ `Any` IS THE DEFAULT POSTURE AND THE BAR FOR LEAVING IT IS HIGH.
 *
 * Ant, 2026-08-08: *"all the fixes should run on the server too, so the server serves the client the
 * correct information"* — and *"this version will run on the server and doesnt no op."*
 *
 * Most of what FPM fixes is SERVER-AUTHORITATIVE: the movement correction the server sends, the RPC
 * the server dispatches, the player state the server matches on join. A fix gated off the server does
 * not degrade gracefully — it does nothing at all, while the log still says the mod loaded. That is
 * the worst available failure shape and it is invisible from the client.
 *
 * So `NeverOnDedicatedServer` is justified only by a SUBSYSTEM THAT DOES NOT EXIST on a server —
 * renderer, audio, input. It is NOT justified by "this feels like a client thing". If a fix's effect
 * reaches the client through replicated state, it belongs on the server, which is where that state is
 * decided.
 *
 * Consequence, already true: `RequiredOnRemote` is `true` in the .uplugin, so SML refuses a join where
 * either side is missing the mod (SMLNetworkManager.cpp:161-193).
 *
 *
 * ★ COMPUTE ON THE AUTHORITY, REPLICATE THE RESULT.
 *
 * Ant, 2026-08-08: *"the client should inherit the servers calculations. the client should only do
 * things only a client needs to do. In a single player world the client is the server so there its
 * just the same. we offload what we can to the server so the client runs faster."*
 *
 * And the reason, which is the load-bearing part: *"if we need to calculate something heavy, make the
 * server do it... the server can focus on the cpu stuff since it doesnt do graphics. that way the
 * client can focus on graphics and not cpu heavy things."*
 *
 * ★ THE ASYMMETRY IS THE WHOLE ARGUMENT. A dedicated server runs no renderer, so its CPU is not
 * competing with a render thread, a draw-call submission path, or a GPU it has to keep fed. The
 * client's CPU is doing exactly that, and it is usually the thing standing between the machine and
 * more frames. So a millisecond of heavy CPU work costs FAR more on the client than the same
 * millisecond costs on the server. Moving work across that boundary is not merely tidy — it converts
 * contended client CPU into uncontended server CPU. This is a PERFORMANCE rule first.
 *
 * ⚠ TWO LIMITS, SO THIS DOES NOT BECOME "OFFLOAD EVERYTHING":
 *  - THE SERVER'S CPU IS NOT FREE, IT IS DIFFERENTLY LOADED. One server carries every player, so work
 *    moved there is multiplied by nobody but paid for by everybody. A server already stalling (the
 *    560 ms save stall) has no headroom to donate.
 *  - SINGLE PLAYER MUST NOT PAY FOR THE SPLIT. Client and server are the same process there, so an
 *    offload must collapse to a direct read — never a replication round-trip that buys nothing and
 *    adds latency. If a design only makes sense with two machines, it is wrong for the majority case.
 *
 * OFFLOADING PAYS WHEN ALL FOUR HOLD:
 *   1. the computation is expensive (raycasts, sweeps, scoring a candidate list),
 *   2. the RESULT is small next to the inputs,
 *   3. the inputs are world state the client would have to reconstruct, and
 *   4. ⚠ THE RESULT TOLERATES LATENCY. Server offload buys uncontended CPU and PAYS A ROUND TRIP.
 *      That rules out anything a frame depends on. Computed-once (the join-time identity match) and
 *      slowly-varying (an enclosure decision at a few Hz) both qualify; a per-frame decision does not.
 * It also rules out anything that is inherently a CLIENT property — GPU headroom, frame timing,
 * resolution scale. The server cannot know those, so the governor decides them locally, always.
 *
 * ⚠ "OFFLOAD TO THE SERVER" AND "OFFLOAD TO A SPARE CORE" ARE DIFFERENT MOVES WITH DIFFERENT HAZARDS.
 * A worker thread costs no latency but hits UE's threading rules — UObject access, actor spawning and
 * most engine APIs are game-thread-only. This project has already paid for that: the RPC gate's census
 * TSet was reachable from Factory Tick workers and unguarded. CSS's own warning is the one to remember
 * — it "could at first look like it's working" until the load balancer moves the tick to another
 * thread. Guard the state, never the fix.
 * In single player the two moves COLLAPSE INTO ONE, because there is no second machine.
 *
 * ⚠ AND THE COUNTER-EXAMPLE, BECAUSE IT IS ALREADY MEASURED AND WILL OTHERWISE BE RE-PROPOSED.
 * Rain occlusion cannot be offloaded, and the old mod's own log proves it. The server-side actor repair
 * already fixed 950+ buildables in one session while the CLIENT still logged 2,226 of the errors the
 * fix exists to prevent — because the client's failing population is LIGHTWEIGHT INSTANCES that are
 * never spawned as actors at all, and are registered from the class through
 * URainOcclusionWorldSubsystem::AddShapeFromClass(..., BuildableCDO, ...). They are not replicable
 * actors on either side. The datum is a bounding box derived from a mesh the client already holds, so
 * replicating it would spend bandwidth to deliver something the client can derive for free — and still
 * miss the objects that need it. Rain needs BOTH halves, server and client, by construction.
 *
 * THE TEST, THEN, IS NOT "is this a client thing" BUT "does the client hold the inputs already?"
 * If it does, computing locally is both cheaper and reachable. If it does not, the authority computes
 * and the client inherits.
 */

/**
 * THE FIX CONTRACT.
 *
 * Every discrete fix is one class, in one file, implementing this. That is the answer to the old mod's
 * 2,300-line module file — not more UE modules, which cost real build-trap risk, but one translation
 * unit per fix inside the one runtime module.
 *
 * IT IS DELIBERATELY FOUR MEMBERS. The design pinned this to an interface plus a review rule: the
 * moment it grows metadata tooling — surface descriptors, dependency graphs, a registry with policy —
 * it has outrun the evidence that anything needs it. The declared HOOK surface already exists for free
 * and cannot drift, because FPMHookLedger records every hook under the owner name the fix passes.
 *
 * A fix is not done when it compiles. It is done when something calls Arm() and a log line proves the
 * hook installed.
 */
class FICSITSPERFORMANCEMANAGER_API IFPMFix
{
public:
	virtual ~IFPMFix() = default;

	/** Stable id. Appears in the arm line and is the owner column of every hook this fix installs. */
	virtual const TCHAR* Name() const = 0;

	/** Read the enum's comment before choosing. The default is Any for a reason. */
	virtual EFPMFixSide Side() const = 0;

	/** Install. Called once, from StartupModule. The ledger refuses the hooks in an editor build. */
	virtual void Arm() = 0;

	/**
	 * Called once per world load, from the root game world module's CONSTRUCTION phase — i.e. while the
	 * loading screen is up.
	 *
	 * WHY THIS EXISTS RATHER THAN EACH FIX DOING ITS OWN WORK LAZILY. Ant, 2026-08-08: "we could just
	 * derive it at runtime ONCE per system when in a loading screen or the benchmark screen. problem
	 * solved. also solves adding new mods." Work that is the SAME every time and expensive to repeat
	 * belongs in one sweep at a moment where a hitch is expected and invisible — not spread across
	 * thousands of lazy per-object hook firings during play.
	 *
	 * Deriving at runtime rather than shipping a precomputed table is also what keeps us clear of
	 * shipping measurements of third-party geometry: the data is produced on the user's machine from
	 * assets they already own.
	 */
	virtual void OnWorldLoad(UWorld* World) {}

	/** Undo. Optional — a log-only fix has nothing to undo. */
	virtual void Disarm() {}
};

namespace FPMFixes
{
	/** Applies the side gate, calls Arm(), logs one line, and remembers the fix so DisarmAll can reach it. */
	FICSITSPERFORMANCEMANAGER_API void Arm(IFPMFix& Fix);

	/** Disarms in reverse arm order, from ShutdownModule. */
	FICSITSPERFORMANCEMANAGER_API void DisarmAll();

	/**
	 * Dispatches OnWorldLoad to every armed fix, in arm order.
	 *
	 * The root game world module calls this. It goes through the registry rather than the module
	 * including feature headers directly — otherwise Module/ grows a dependency on every fix that ever
	 * wants load-time work, which is the coupling the old mod's structure died of.
	 */
	FICSITSPERFORMANCEMANAGER_API void NotifyWorldLoad(UWorld* World);
}
