// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

#include "Core/FPMDiag.h"

/**
 * ★ WHAT KIND OF CLAIM A FIX IS MAKING — Ant's Q1 ruling, in structural form (design §2.2).
 *
 * The word "fixed" was being used for work that repaired a SYMPTOM at a convenient place without ever
 * naming the CAUSE. That is not dishonesty, it is drift: every individual case reads as reasonable and
 * the corpus ends up claiming more than it earned. Making the distinction a compile-time obligation is
 * what stops it, because an author cannot ship without answering the question.
 *
 * ⚠ DELIBERATELY FOUR VALUES. The moment this wants sub-states it has outrun its evidence — the same
 * discipline the fix contract itself is pinned to.
 */
enum class EFPMOriginStatus : uint8
{
	/** The CAUSE is identified with a receipt. This is the only value beside which "fixed" may be used. */
	OriginNamed,

	/** Repairs at the earliest reachable point; the cause is not yet named. */
	ChokePointRepair,

	/** Prevents a harm from a cause we may never own — another mod, or the engine. */
	Guard,

	/** Symptom handled, mechanism not understood. Highest scrutiny, and it must carry a diagnostic. */
	UnknownCause,
};

/** For the armed line and `FPM.Diag.Dump`. */
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMOriginStatus Status);

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
 * ★ IT IS NOW EIGHT MEMBERS, AND THE FREEZE IS RE-STATED RATHER THAN QUIETLY BROKEN.
 *
 * It used to say "DELIBERATELY FOUR MEMBERS", then a later pass rewrote that to "SIX MEMBERS" without
 * a fresh count against the class body. The real count today is `Name()`, `Side()`, `OriginStatus()`,
 * `Channel()`, `Arm()`, `OnWorldLoad()`, `Disarm()`, `DefaultArmed()` - eight, counted by hand against
 * the class below. `DefaultArmed()` alone was "Added 2026-08-11 because measurement forced it" (its
 * own comment, below), which is what took six to eight; a comment contradicting the code it sits on is
 * the project's own named defect, and leaving a stale freeze note is how a rule stops being believed.
 *
 * Two of the eight are Ant-ruled, not author-chosen: `OriginStatus()` (her Q1 ruling, a fix must
 * declare what kind of claim it is making) and `Channel()` (design §3.1, a fix must declare where its
 * diagnostics go). Both are PURE VIRTUAL on purpose. A default would let a new fix inherit somebody
 * else's answer silently, and the entire value here is that the author cannot avoid answering.
 *
 * THE FREEZE ITSELF STANDS, unchanged in substance: the moment this grows metadata tooling — surface
 * descriptors, dependency graphs, a registry with policy — it has outrun the evidence that anything
 * needs it. The declared HOOK surface still comes for free and still cannot drift, because
 * FPMHookLedger records every hook under the owner name the fix passes.
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

	/**
	 * WHAT KIND OF CLAIM THIS FIX MAKES. Printed on the armed line and in `FPM.Diag.Dump`, and it governs
	 * the changelog: **"fixed" may only appear beside `OriginNamed`.** Anything else is named as what it
	 * actually is, and anything that is not `OriginNamed` owes an origin-naming diagnostic — a channel
	 * whose stated job is to name the cause from play data.
	 */
	virtual EFPMOriginStatus OriginStatus() const = 0;

	/** Where this fix's diagnostics go. Pure virtual so a new fix cannot inherit somebody else's channel. */
	virtual FPMDiag::EChannel Channel() const = 0;

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

	/**
	 * ★ SHOULD THIS FIX BE ARMED WITHOUT THE PLAYER ASKING? Default yes. A fix answering NO is still
	 * registered, listed and toggleable — it simply installs nothing until `FPM.Fix.<Name> 1`.
	 *
	 * Added 2026-08-11 because measurement forced it. `no-owner-rpc-gate` suppressed ZERO dispatches on
	 * Ant's client AND on her dedicated server, and the engine warning it exists to pre-empt
	 * ("No owning connection for actor") appeared 0 times server-side and once client-side — on an actor
	 * class the gate deliberately skips. It cancelled nothing, while holding a funchook detour on
	 * `UNetDriver::ProcessRemoteFunction`, which every RPC in the game passes through. She measured
	 * `FPM.Fix.NoOwnerRpcGate 0` restoring her hoverpack audio and animation.
	 *
	 * A fix with no measurable work and a measured cost must not be on by default. This is NOT deletion:
	 * the Stats-sign flood that motivated it was real and may come back, and it is one cvar away.
	 *
	 * ⚠ READ IN TWO PLACES, AND THEY MUST AGREE — `FPMFixes::Arm` for the initial install and
	 * `FPMFixToggles::Install` for the cvar's default. Both call THIS, so there is one declaration site;
	 * a second copy of the default is a bug by construction and this project has shipped that before.
	 */
	virtual bool DefaultArmed() const { return true; }
};

/**
 * ONE LOG-THROTTLE POLICY FOR EVERY FIX, STATED ONCE.
 *
 * The divisor encodes how EXPECTED an event is — not how much we happen to want to see it. Before this
 * existed, five sites across two fixes used 50 / 100 / 200 as bare literals with no stated rule, which
 * is how a log becomes unreadable by accretion and how two readers draw opposite conclusions from the
 * same gap between lines.
 *
 * A third tier is deliberately NOT here. An event believed unreachable is logged UNTHROTTLED, because
 * every occurrence is a finding — and a `% 1` divisor would be dead code dressed as policy.
 *
 * ⚠ IT LIVES IN THIS HEADER, NOT IN EACH .cpp, AND THAT IS NOT A STYLE CHOICE. UE builds this module as
 * a UNITY BUILD: the .cpp files are concatenated into one translation unit, so two anonymous namespaces
 * declaring the same name are one namespace declaring it twice — `error C2374: redefinition`. File-local
 * constants are not file-local here. Anything shared belongs in a header; anything genuinely per-fix
 * needs a name that is unique across the whole module.
 */
namespace FPMLog
{
	/** Happens constantly and correctly — sample it. */
	inline constexpr int32 ThrottleRoutine = 200;

	/** Happens sometimes and is worth watching. */
	inline constexpr int32 ThrottleNotable = 50;
}

namespace FPMFixes
{
	/** Applies the side gate, calls Arm(), logs one line, and remembers the fix so DisarmAll can reach it. */
	FICSITSPERFORMANCEMANAGER_API void Arm(IFPMFix& Fix);

	/**
	 * Disarms in reverse arm order. Called from ShutdownModule, and from the P4.2 master switch.
	 *
	 * It empties the ARMED list but keeps the REGISTRY, which is what lets `RearmAll` put everything
	 * back. Before 0.11.9 this was effectively a no-op for 13 of 28 fixes — they never overrode
	 * `Disarm()` — so it reported them disarmed while their hooks stayed live.
	 */
	FICSITSPERFORMANCEMANAGER_API void DisarmAll();

	/**
	 * Re-arms every registered fix that is not currently armed. The other half of the master switch.
	 *
	 * ⚠ It arms only what is NOT already armed, and that guard is load-bearing rather than defensive:
	 * most `Arm()` bodies subscribe unconditionally, so calling one twice installs a SECOND handler on
	 * the same method — a cancelling fix would cancel twice, a counting one would double every number.
	 * The armed state lives in the registry precisely so no fix has to be rewritten to be idempotent.
	 *
	 * ⚠ A fix whose real work happens in `OnWorldLoad` comes back armed but INERT until the next world
	 * load, because re-arming does not replay one. The log line says so rather than implying otherwise.
	 */
	FICSITSPERFORMANCEMANAGER_API void RearmAll();

	/**
	 * Arms or disarms ONE registered fix. The P4.3 per-fix toggles run through this.
	 *
	 * Same reason `RearmAll` exists: the armed state is the registry's, not the fix's, so this can never
	 * double-subscribe a fix whose `Arm()` is not idempotent.
	 *
	 * @return true if the state actually CHANGED. False means it was already in that state — which the
	 *         caller must not report as a toggle, or a log fills with disarms that disarmed nothing.
	 */
	FICSITSPERFORMANCEMANAGER_API bool SetArmed(IFPMFix& Fix, bool bWantArmed);

	/** Every fix that passed the side gate, armed or not. The toggles enumerate this. */
	FICSITSPERFORMANCEMANAGER_API const TArray<IFPMFix*>& Registered();

	/** True when this fix is armed right now. */
	FICSITSPERFORMANCEMANAGER_API bool IsArmed(const IFPMFix& Fix);

	/**
	 * Proves the registry's own invariants at boot, the way `FPMCVarWriter::SelfTest` proves the write
	 * path on a probe cvar instead of asserting it works.
	 *
	 * ★ EVERY CHECK IS NON-MUTATING. It does not arm, disarm or cycle a real fix — a self-test that
	 * changes the thing it measures is not a test, it is a side effect with a log line. What it can
	 * prove without touching state is the part that would otherwise fail silently:
	 *
	 *   1. `SetArmed` REFUSES a fix that is not registered. That is the side gate's back door, and if it
	 *      ever opens, a dedicated server can arm a client-only fix through a toggle.
	 *   2. `SetArmed` reports NO CHANGE when asked for the state a fix is already in, so a no-op set
	 *      never logs a toggle that toggled nothing.
	 *   3. `Armed()`, `Registered()` and `IsArmed()` AGREE. Three accessors over two arrays is exactly
	 *      how an inventory starts lying, and the inventory lying is what `FPMHookLedger` exists to stop.
	 *   4. THE REACHED COUNTING WRAPPER forwards every argument unchanged and counts exactly one per
	 *      call, in all four `FPM_SUBSCRIBE` families, and every INSTALLED hook carries it. A wrapper
	 *      that drops an argument or that counts at install time fails silently, and the result is a
	 *      working hook with a wrong number beside it. The classifier is proved both ways, against an
	 *      unwrapped slot and a wrapped one, the way `FPMCVarWriter::SelfTest` proves its write path.
	 *
	 * ⚠ WHAT IT DOES NOT PROVE, said out loud: that a real arm/disarm CYCLE works, and that any hooked
	 * function ever runs. Both need a boot. Until one happens every REACHED count is 0, and that 0 is
	 * arithmetic rather than evidence. `FPM.Hooks.Report` reads them after play.
	 *
	 * @return true if every check passed.
	 */
	FICSITSPERFORMANCEMANAGER_API bool SelfTest();

	/**
	 * Dispatches OnWorldLoad to every armed fix, in arm order.
	 *
	 * The root game world module calls this. It goes through the registry rather than the module
	 * including feature headers directly — otherwise Module/ grows a dependency on every fix that ever
	 * wants load-time work, which is the coupling the old mod's structure died of.
	 */
	FICSITSPERFORMANCEMANAGER_API void NotifyWorldLoad(UWorld* World);

	/** `FPM.Diag.Dump` -- every armed fix with its side, origin status and channel, then every channel. */
	FICSITSPERFORMANCEMANAGER_API void Dump();

	/**
	 * The armed fixes, in arm order.
	 *
	 * Exists because `Dump()` writes to the LOG, and §7.1's support bundle has to write the same inventory
	 * to an arbitrary `FOutputDevice` so it lands in the console where a player can copy it. Rather than
	 * let the support bundle keep its own second list — which is how two inventories drift apart and one
	 * of them starts lying — both read this.
	 *
	 * The pointers are to function-local statics owned for the life of the process; nothing here manages a
	 * lifetime, and the reference is only valid on the thread that armed them (the game thread).
	 */
	FICSITSPERFORMANCEMANAGER_API const TArray<IFPMFix*>& Armed();
}
