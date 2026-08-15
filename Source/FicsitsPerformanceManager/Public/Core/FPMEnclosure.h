// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ AM I INSIDE? ASKED ONCE, ANSWERED ONCE, FOR EVERY FEATURE THAT NEEDS IT.
 *
 * Ant, 2026-08-10: *"Yes, one check for all \"inside\" stuff."* · *"It needs to check for sealed rooms,
 * not just cover."* · *"it needs to be performant. It's a performance mod after all."*
 *
 * Three features want this answer — the indoor fog, the HUD visor rain droplets, and the airborne
 * particles that currently drift through walls. Giving each its own test is the wrong build, and not
 * for tidiness: three tests drift, and then two features disagree about whether the player is inside
 * while each looks reasonable on its own. One ray batch, one cadence, one damped verdict.
 *
 * ══ WHY THE OLD PROBE COULD NOT DO THIS ══
 *
 * FPM1's enclosure test was **sky-biased**: thirteen rays aimed up, weighted toward the zenith. It can
 * find a roof and it is structurally blind to walls. That is fine for a fog bubble and useless for
 * "why do particles come through my wall", which is the fault Ant is actually reporting. So the ray set
 * is rebuilt rather than carried.
 *
 * ══ THE RAY SET ══
 *
 * 24 directions on a **Fibonacci hemisphere**, `z >= -0.15`. Even angular coverage with no axis bias,
 * which hand-placed rings cannot give and which matters once walls count.
 *
 * ⚠ THE FLOOR IS DELIBERATELY EXCLUDED. Straight down is blocked whenever the player is standing on
 * anything, so it is a constant, and a constant carries no information while still costing a trace and
 * inflating every fraction. Walls are found by the horizontal band, not by looking down.
 *
 * ══ WHAT IT REPORTS, AND WHY MORE THAN ONE NUMBER ══
 *
 * The consumers do NOT want the same predicate, and forcing one on them would be a bug dressed as
 * consistency:
 *
 *   - **Fog** counts PLAYER BUILDABLES only. A natural cave ceiling should stay foggy — a deliberate
 *     ruling on the old controller, and right.
 *   - **Visor rain** needs only a roof. Vanilla already handles world geometry through its baked static
 *     occlusion textures, so the part vanilla misses is player buildables overhead.
 *   - **Particles** need a genuinely sealed volume, walls included.
 *
 * One batch produces all of it. There is still exactly one place the geometry question is asked.
 *
 * ══ PERFORMANCE — THIS IS A PERFORMANCE MOD ══
 *
 * Four measures, in decreasing order of what they save:
 *
 *  1. **Nothing runs when nothing is listening.** No consumer registered, no traces. Zero, not cheap.
 *  2. **Nothing runs while the player is still.** The ray set is world-space, so the reading depends on
 *     POSITION and not on where the camera is pointing. Standing at a machine, turning on the spot,
 *     reading a sign — all reuse the last answer. This is the largest saving by far, because it is the
 *     common case.
 *  3. **The traces are ASYNC.** `UWorld::AsyncLineTraceByChannel` (`World.h:2372`) queues them onto the
 *     physics scene's own worker threads and delivers the results through a delegate on the game
 *     thread next frame. The game thread never blocks on a trace.
 *  4. **Capped cadence** even while moving.
 *
 * ⚠ IT IS DELIBERATELY *NOT* OFFLOADED TO THE SERVER, and that is the design's own rule rather than a
 * shortcut. Offload pays only when the client does not already hold the inputs. Here the input is the
 * local camera position and the local collision scene — the client has both, so a round trip would add
 * latency and buy nothing. The design states this directly in its rain counter-example. A doorway also
 * needs a sub-second response, which rules out anything with a round trip.
 *
 * ⚠ THE SERVER REFUSAL IS EXPLICIT, NOT INCIDENTAL. §9.7 named the old wording here — "client-side by
 * nature" — as FALSE read as a refusal claim: the no-op was only ever a SIDE EFFECT of
 * `GetFirstLocalPlayerController` returning null on a dedicated server, which happens to produce the
 * right behaviour today but documents the wrong reason and would silently stop refusing if that lookup
 * ever changed. `TickInternal` now checks `IsRunningDedicatedServer()` first, before anything else runs.
 *
 * ZERO RESIDUE: line traces and integers. No console variable, no ini, no actor touched, nothing
 * written to any object in the world.
 */
struct FFPMEnclosureReading
{
	/** ★ SEALED ROOM: fraction of the hemisphere blocked by PLAYER BUILDABLES, walls included. */
	float BuiltSealed = 0.f;

	/** Fraction of the UPWARD rays blocked by player buildables. A roof, ignoring walls. */
	float BuiltOverhead = 0.f;

	/** Fraction of the hemisphere blocked by anything at all, terrain and world assets included. */
	float AnySealed = 0.f;

	/** Distinct rays that hit a buildable. Stops one large nearby surface carrying a fraction alone. */
	int32 BuiltHits = 0;

	/** Distance to the nearest sealing hit, cm. -1 when nothing was hit. Sizes a fog bubble. */
	float NearestCm = -1.f;

	/** False when there was no pawn to trace from — menu, spectator, or mid-respawn. */
	bool bValid = false;
};

/** What a consumer is asking about. Registering one is what turns the sampler on. */
enum class EFPMEnclosureNeed : uint8
{
	/** A roof overhead. Cheapest predicate. The visor-rain gate wants this. */
	Overhead,

	/** A sealed volume, walls included. Particles and fog want this. */
	SealedRoom,
};

class FICSITSPERFORMANCEMANAGER_API FPMEnclosure
{
public:
	/**
	 * Register a consumer. The sampler runs only while at least one is registered, so a disabled
	 * feature costs literally nothing rather than costing a little.
	 *
	 * @return a token to hand back to `Unregister`.
	 */
	static int32 Register(const TCHAR* ConsumerName, EFPMEnclosureNeed Need);
	static void Unregister(int32 Token);

	/** Stops sampling and forgets every consumer. Called from module shutdown. */
	static void Shutdown();

	/**
	 * ★ THE DAMPED VERDICTS. These are what a consumer reads.
	 *
	 * Damped because walking a doorway makes a raw reading flicker, and an undamped consumer would
	 * pulse its effect. The damping is SYMMETRIC on purpose: being late to hide an effect shows the
	 * bug, being late to show it looks like a missing effect, and neither is clearly worse — so neither
	 * direction gets the benefit of the doubt.
	 */
	static bool IsUnderBuiltRoof();
	static bool IsInSealedRoom();

	/** The most recent raw reading, undamped — for a log line or a consumer with its own thresholds. */
	static const FFPMEnclosureReading& Last();

	/** Seconds since the last completed reading. Large means the sampler is idle or the player is still. */
	static double SecondsSinceReading();

	/**
	 * ★ THE WORLD CHANGED NEAR THE PLAYER — force the next tick to re-probe.
	 *
	 * Ant, 2026-08-10: *"what if another player builds something around the first player without the
	 * first moving? that would break the system."* It did. The movement skip watches the CAMERA, and a
	 * wall appearing around a stationary player is an input change no amount of position-watching sees.
	 * The two failures are not symmetric: being walled in leaves weather playing indoors, and being
	 * un-roofed leaves weather suppressed while you stand in the rain. The second reads as a broken mod.
	 *
	 * `GFPMMaxCacheAgeSec` is the floor that bounds staleness at two seconds. This is the fast path: a
	 * buildable finishing `BeginPlay` within `RadiusCm` of the last probe origin invalidates immediately,
	 * so being walled in registers next tick rather than up to two seconds later.
	 *
	 * ⚠ IT TAKES A LOCATION ON PURPOSE. `AFGBuildable::BeginPlay` fires for EVERY buildable, and a
	 * blueprint paste or a world load is thousands of them. Invalidating unconditionally would force a
	 * re-probe every tick for the whole of a build session. The distance test lives HERE rather than at
	 * the call site, because only this module knows where it last probed from.
	 *
	 * ⚠ AND IT DOES NOT CLEAR THE LAST READING. Consumers keep the previous answer until the new probe
	 * lands. Blanking it would flicker every downstream effect for one probe interval — a visible
	 * regression traded for no correctness gain, since the reading is stale rather than wrong-shaped.
	 *
	 * Cheap enough to call per buildable: one squared-distance compare against a cached vector.
	 */
	static void InvalidateNear(const FVector& WorldLocation, float RadiusCm);

	/**
	 * `FPM.Enclosure.Report`: the reading, both verdicts, and what it cost. On-demand only, it prints
	 * unconditionally whenever it is called, the same as every other FPM `*.Report` command, so it is
	 * not gated by `FPM.Diag.Enclosure`.
	 *
	 * This is NOT how a verdict flip reaches the log during normal play. That happens on its own, gated
	 * at `FPM.Diag.Enclosure` level 1 (the default), the moment `IsUnderBuiltRoof()` or `IsInSealedRoom()`
	 * actually changes, with a rare heartbeat so a long quiet stretch cannot be mistaken for a dead
	 * watcher. Review 2026-08-15: this command used to be the ONLY place the verdict was visible, which
	 * meant whatever was calling it stood in for a proper change log and, per the session that motivated
	 * this review, called it often enough to produce most of everything FPM printed that session.
	 */
	static void LogNow();

};
