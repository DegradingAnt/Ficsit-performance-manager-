// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMIndoorFog.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMEnclosure.h"
#include "Core/FPMHookLedger.h"

#include "FGAtmosphereUpdater.h"
#include "FGWorldSettings.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

/*
 * THE BEHAVIOUR SWITCH. Separate from FPM.Diag.IndoorFog, which only changes what is printed.
 *
 * 0 disables the push while leaving the hook installed, the enclosure consumer registered and the
 * reporting live, so one boot can A/B whether this is what cleared the haze, without a rebuild.
 *
 * ⚠ WHAT 0 DOES **NOT** KEEP MEASURING: the re-drive count. It only moves while FPM is holding a value
 * for the game to overwrite, and the gate at 0 means FPM holds nothing. So `FPM.Fog.Gate 0` reports the
 * under-roof state and the game's own StartDistance and stops answering "does the game own this
 * property". Say that here rather than let a reader take a frozen counter for a measured zero.
 */
static TAutoConsoleVariable<int32> CVarIndoorFogGateEnabled(
	TEXT("FPM.Fog.Gate"), 1,
	TEXT("Push the height fog's StartDistance back while the player is under a built roof. "
	     "1 = on (default), 0 = observe and report but change nothing."),
	ECVF_Default);

/*
 * UNMEASURED, ON PURPOSE — see FPMIndoorFog.h. StartDistance's own UIMax
 * (ExponentialHeightFogComponent.h) is 5000 cm, a slider hint rather than a clamp, and a factory
 * interior easily exceeds that. 20000 cm (200 m) is a starting guess wide enough to clear a large hall
 * without being unreachable for a small room; one boot standing in a real base answers this properly,
 * the same way FPM.Enclosure.StreakToFlip was tuned.
 */
static TAutoConsoleVariable<float> CVarIndoorFogStartDistance(
	TEXT("FPM.Fog.IndoorStartDistance"), 20000.f,
	TEXT("StartDistance (cm) to hold while under a built roof. UNMEASURED default - raise or lower it "
	     "live and watch FPM.Fog.Report until the haze clears without over-reaching."),
	ECVF_Default);

namespace
{
	int32 GFPMFogEnclosureToken = INDEX_NONE;
	FDelegateHandle GFPMFogTickHandle;

	/*
	 * The resolved fog component, cached against the world it came from. Both are weak: a level change
	 * tears down the world and the component with it, and a stale raw pointer would be a write into
	 * freed memory. The world pointer is compared rather than dereferenced, so a torn-down world can
	 * only ever cause a re-resolve, never a use.
	 */
	TWeakObjectPtr<UWorld> GFPMFogCachedWorld;
	TWeakObjectPtr<UExponentialHeightFogComponent> GFPMFogCachedComp;

	bool GFPMFogUnderRoof = false;
	bool GFPMFogHolding = false;

	/*
	 * ★ THE GAME'S OWN VALUE, AND WHY IT IS SAMPLED HERE RATHER THAN ONCE AT THE START.
	 *
	 * This handler runs immediately after UFGAtmosphereUpdater::Tick has finished the frame's fog work,
	 * so whatever is in StartDistance at the top of this handler is the game's own value for this frame
	 * — UNLESS the game did not touch it, in which case it is still ours from last frame. Those two
	 * cases are told apart below by comparing against GFPMFogWroteValue, and only the first updates
	 * this. So the restore point is re-read from the game every frame and can never be a value FPM
	 * itself put there. The old design shadowed it once and then had to reason about whether the shadow
	 * had been contaminated; there is nothing to reason about now.
	 */
	float GFPMFogGameValue = 0.f;

	/** What we last wrote, so the sample above can tell the game's value from our own. */
	float GFPMFogWroteValue = 0.f;

	/*
	 * ★ THE THREE COUNTERS, AND THE INPUT THAT MOVES EACH ONE.
	 *
	 * A counter nobody can move is a certificate, not a measurement, so each of these is named with the
	 * concrete thing that changes it. A fourth was designed and then DELETED: read the write back on the
	 * next line and count the mismatches. The engine's setter is generated as
	 * `if (Name != NewValue) { Name = NewValue; ... }` with no other early exit
	 * (ExponentialHeightFogComponent.cpp:231-240), so the read-back can only ever equal what was just
	 * written. No input makes it report anything but zero, which is the definition of a dead instrument.
	 *
	 * The fourth number that DOES matter is not kept here at all: whether the hook ever fired. That is
	 * FPMHookLedger's REACHED column for UFGAtmosphereUpdater::Tick, printed by FPM.Hooks.Report, and it
	 * is what separates "the push never had a chance to run" from "it ran and had nothing to do".
	 */

	/** Frames we wrote StartDistance. Moves when the player is under a built roof with the gate on. */
	int32 GFPMFogPushes = 0;

	/**
	 * ★ THE ONE THAT TESTS THE PREMISE. Frames where, while holding, StartDistance had changed away
	 * from our value by the time this handler ran — i.e. the game re-drove it. It moves in a world
	 * whose atmosphere volumes drive StartDistance from a curve, and it STAYS AT ZERO in a world that
	 * does not. A zero here next to a climbing Pushes is the finding that the whole redesign rests on a
	 * false premise and the original 4 Hz ticker should have worked.
	 */
	int32 GFPMFogRedrives = 0;

	/**
	 * Frames spent holding, written or not. `FramesHeld - Pushes` is the number of frames the property
	 * already held our number and nothing had to be written. Moves whenever the player is under a built
	 * roof with the gate on.
	 */
	int32 GFPMFogFramesHeld = 0;

	/**
	 * AFGWorldSettings -> AExponentialHeightFog -> its component, cached against the world it came from.
	 *
	 * The cache is not a micro-optimisation looking for a job. This runs once per rendered frame now,
	 * where the old ticker ran four times a second, and `AFGWorldSettings::GetExponentialHeightFog()` is
	 * declared out of line (FGWorldSettings.h:72) so its cost is NOT knowable from the header drop. A
	 * cold resolve happens when the world changes or the component dies; a map with no fog actor caches
	 * a null and therefore re-resolves every frame, which is deliberate — the actor may stream in later.
	 */
	UExponentialHeightFogComponent* FogComponentFor(UWorld* World)
	{
		if (GFPMFogCachedWorld.Get() != World || !GFPMFogCachedComp.IsValid())
		{
			GFPMFogCachedWorld = World;

			const AFGWorldSettings* WS = World ? Cast<AFGWorldSettings>(World->GetWorldSettings()) : nullptr;
			AExponentialHeightFog* FogActor = WS ? WS->GetExponentialHeightFog() : nullptr;
			GFPMFogCachedComp = FogActor ? FogActor->GetComponent() : nullptr;
		}
		return GFPMFogCachedComp.Get();
	}

	/*
	 * Forget both per-world flags, WITHOUT touching a component. Called only when there is no play
	 * world or no fog actor to restore onto, which is also the only time it is safe: the hold flag
	 * carries a restore obligation, and dropping it anywhere else would leave our value on a live
	 * component with nothing scheduled to take it off.
	 *
	 * It must not be a plain `return`. GFPMFogGameValue belongs to the world that supplied it, so
	 * carrying a hold across a level change would write the OLD map's fog distance onto the NEW map's
	 * component the first frame the player steps outside. The roof latch goes with it so the next
	 * world logs its own first transition rather than inheriting one.
	 */
	void ForgetWorldState()
	{
		GFPMFogHolding = false;
		GFPMFogUnderRoof = false;
	}

	/*
	 * ★ THE WHOLE FIX, AND IT IS ORDERING, NOT FORCE.
	 *
	 * Called from an _AFTER handler on UFGAtmosphereUpdater::Tick, which SML runs once the real Tick
	 * has returned (NativeHookManager.h:442-451, ApplyCallVoid: the trampoline first, the AFTER
	 * handlers second). So FPM is the last game-thread writer of StartDistance in the frame, every
	 * frame, and the renderer sees our value at the end-of-frame flush. The previous design wrote from
	 * a 4 Hz ticker with no defined position relative to the updater, which is why the live value
	 * alternated and Ant saw the light pulse indoors.
	 */
	void PushAfterAtmosphereUpdate()
	{
		UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
		if (World == nullptr || !World->IsGameWorld()) { ForgetWorldState(); return; }

		UExponentialHeightFogComponent* Comp = FogComponentFor(World);
		if (Comp == nullptr) { ForgetWorldState(); return; }   // no fog actor in this map; not a fault

		const float Live = Comp->StartDistance;

		/*
		 * ⚠ THE COMPARISON IS EXACT-ish (KINDA_SMALL_NUMBER), NOT THE OLD 1 cm TOLERANCE.
		 *
		 * We wrote GFPMFogWroteValue ourselves, so if nothing else touched the property the bits come
		 * back identical. A 1 cm window would swallow a re-drive that happened to land within 1 cm of
		 * our target and silently report the premise as false. The one case this misreads is a game
		 * value that IS our target — the map's own curve holds 8.710419 against a 20000 cm target, so
		 * that costs one un-updated restore point and nothing else.
		 */
		const bool bLiveIsOurs = GFPMFogHolding && FMath::IsNearlyEqual(Live, GFPMFogWroteValue, KINDA_SMALL_NUMBER);
		if (!bLiveIsOurs)
		{
			GFPMFogGameValue = Live;
			if (GFPMFogHolding) { ++GFPMFogRedrives; }
		}

		/*
		 * bValid is false in a menu, while spectating and mid-respawn (FPMEnclosure.h:93). The verdict
		 * flags themselves are NOT cleared on those paths — FPMEnclosure.cpp:504-510 invalidates the
		 * reading and returns — so asking IsUnderBuiltRoof() alone would hold a stale YES through a
		 * respawn and keep the fog pushed with no player under any roof.
		 */
		const bool bUnderRoof = FPMEnclosure::Last().bValid && FPMEnclosure::IsUnderBuiltRoof();
		const bool bEnabled = CVarIndoorFogGateEnabled.GetValueOnGameThread() != 0;
		const float Target = CVarIndoorFogStartDistance.GetValueOnGameThread();

		if (bUnderRoof != GFPMFogUnderRoof)
		{
			GFPMFogUnderRoof = bUnderRoof;
			if (FPMDiag::IsOn(FPMDiag::EChannel::IndoorFog))
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] indoor fog: under built roof = %s (gate %s)"),
					bUnderRoof ? TEXT("YES") : TEXT("no"),
					bEnabled ? TEXT("on") : TEXT("OFF - observing only"));
			}
		}

		if (bUnderRoof && bEnabled)
		{
			++GFPMFogFramesHeld;

			/*
			 * ⚠ THE `!=` TEST IS NOT DEFENSIVE PADDING, IT IS THE COST CONTROL. The engine setter dirties
			 * the render state on every value CHANGE (ExponentialHeightFogComponent.cpp:231-240), and
			 * MarkRenderStateDirty collapses repeats within a frame but not across them
			 * (ActorComponent.cpp:2550-2564). So a frame where the property already holds our number must
			 * not call the setter at all: that is the difference between one fog render-state recreate
			 * per frame and none.
			 */
			if (!FMath::IsNearlyEqual(Live, Target, KINDA_SMALL_NUMBER))
			{
				Comp->SetStartDistance(Target);
				++GFPMFogPushes;
			}

			GFPMFogWroteValue = Target;
			GFPMFogHolding = true;
		}
		else if (GFPMFogHolding)
		{
			/*
			 * ZERO RESIDUE, and it is one line because the restore point was re-read from the game this
			 * very frame. Walked outside, gate turned off, or lost the pawn — all three land here, once,
			 * and from the next frame neither branch touches StartDistance. The game would put its own
			 * value back on its next Tick regardless; this makes it immediate rather than relying on it.
			 */
			Comp->SetStartDistance(GFPMFogGameValue);
			GFPMFogHolding = false;
		}
	}
}

FFPMIndoorFog& FFPMIndoorFog::Get()
{
	static FFPMIndoorFog Instance;
	return Instance;
}

void FFPMIndoorFog::Arm()
{
	/*
	 * Overhead, not SealedRoom: the predicate IS a roof (see the header — a natural cave ceiling should
	 * stay foggy, only a player-built one should not). Registering Overhead does not by itself force the
	 * wall band to trace; the sampler only adds that cost if some OTHER consumer also needs SealedRoom.
	 */
	GFPMFogEnclosureToken = FPMEnclosure::Register(TEXT("indoor-fog"), EFPMEnclosureNeed::Overhead);

	// Named first, not left as the macro's inline argument — FPMHookLedger.h's own warning: the handler
	// must not contain a top-level comma, and naming it here removes the question entirely.
	auto OnAtmosphereTicked = [](UFGAtmosphereUpdater* /*Updater*/, float /*Dt*/)
	{
		PushAfterAtmosphereUpdate();
	};

	GFPMFogTickHandle = FPM_SUBSCRIBE_AFTER("indoor-fog", UFGAtmosphereUpdater::Tick, OnAtmosphereTicked);

	/*
	 * ⚠ THE ARMED LINE IS BRANCHED, because in an editor build there is no hook to describe.
	 * FPMHookLedger::Install is `if constexpr (WITH_EDITOR)` -> record REFUSED, return an empty handle
	 * (FPMHookLedger.cpp:60-70). An unconditional "ARMED, hooks UFGAtmosphereUpdater::Tick" would then
	 * be a false claim printed on every editor boot, and this fix has now given up the ticker that used
	 * to make it work there. The ledger prints its own REFUSED warning; this says what it means FOR
	 * THIS FIX, which is that nothing will move.
	 */
	if (GFPMFogTickHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] indoor fog ARMED - hooks UFGAtmosphereUpdater::Tick AFTER, so FPM writes the "
			     "map's height fog StartDistance last in the frame instead of racing the game's own "
			     "writer. It pushes StartDistance to %.0f cm while the shared enclosure check reports a "
			     "built roof overhead. SetStartDistance ONLY - volumetric fog is never touched. "
			     "FPM.Fog.Report carries the counts, and its re-drive line is what says whether the "
			     "game drives this property at all in this world."),
			CVarIndoorFogStartDistance.GetValueOnAnyThread());
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] indoor fog INERT - the hook on UFGAtmosphereUpdater::Tick did not install, so "
			     "nothing writes StartDistance. In an editor build that is expected and the enclosure "
			     "consumer is still registered for its own reporting. In a packaged build it is a "
			     "failure: read SML's hook log for the refusal reason."));
	}
}

void FFPMIndoorFog::Disarm()
{
	/*
	 * ⚠ RESTORE BEFORE UNHOOKING, not after. The restore needs the fog component, and resolving it
	 * costs nothing here — but doing it in this order also means that if the restore throws the fix
	 * off the happy path, the hook is still installed and the next frame corrects the value anyway.
	 */
	if (GFPMFogHolding)
	{
		if (UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr)
		{
			if (UExponentialHeightFogComponent* Comp = FogComponentFor(World))
			{
				Comp->SetStartDistance(GFPMFogGameValue);
			}
		}
		GFPMFogHolding = false;
	}

	// IsValid() guarded: the editor path returns an invalid handle, and UNSUBSCRIBE on one is not free.
	if (GFPMFogTickHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UFGAtmosphereUpdater::Tick, GFPMFogTickHandle);
		GFPMFogTickHandle.Reset();
	}

	GFPMFogCachedWorld.Reset();
	GFPMFogCachedComp.Reset();

	if (GFPMFogEnclosureToken != INDEX_NONE)
	{
		FPMEnclosure::Unregister(GFPMFogEnclosureToken);
		GFPMFogEnclosureToken = INDEX_NONE;
	}
}

void FFPMIndoorFog::ReportNow()
{
	/*
	 * %.2f on every MEASURED StartDistance. The first report of this bug read "read back 9", and 9
	 * matches nothing in the game's data. The value is 8.710419, the constant held by the map's own
	 * Main_Middle_StartDistance curve, and the rounding is the only reason that took a search to find.
	 * The cvar target keeps %.0f: an operator typed it.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] indoor fog: under-roof=%s * gate %s * holding=%s * game's own StartDistance %.2f cm "
		     "* indoor target %.0f cm"),
		GFPMFogUnderRoof ? TEXT("YES") : TEXT("no"),
		CVarIndoorFogGateEnabled.GetValueOnGameThread() != 0 ? TEXT("on") : TEXT("OFF"),
		GFPMFogHolding ? TEXT("yes") : TEXT("no"),
		GFPMFogGameValue, CVarIndoorFogStartDistance.GetValueOnGameThread());

	if (GFPMFogFramesHeld == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   never held yet - the player has not been under a built roof with the gate on, "
			     "no fog actor exists in this map, or the hook on UFGAtmosphereUpdater::Tick is not "
			     "running. FPM.Hooks.Report separates the last one from the other two."));
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   %d frame(s) held: %d written, %d already at target. Each written frame costs one "
		     "fog render-state recreate; the rest cost none."),
		GFPMFogFramesHeld, GFPMFogPushes, GFPMFogFramesHeld - GFPMFogPushes);

	/*
	 * ★ THIS LINE IS THE POINT OF THE REPORT, and it is not behind FPMDiag: an operator who asked for
	 * this report by name is asking whether the fix is doing anything, and Redrives is the answer.
	 *
	 * Redrives climbing means the game re-writes StartDistance every frame and being the last writer is
	 * exactly what this fix has to be. Redrives at zero across many held frames means the game is NOT
	 * driving this property in this world, the pulse Ant saw had some other cause, and the hook is
	 * buying nothing over a plain ticker. Say which one it is rather than making the reader infer it.
	 */
	if (GFPMFogRedrives > 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %d re-drive(s): the game moved StartDistance off our value that many times, so "
			     "it does own this property and writing last is what makes the push hold."),
			GFPMFogRedrives);
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   ZERO re-drives across %d held frame(s). The game did not move StartDistance "
			     "once, which contradicts the reason this fix hooks UFGAtmosphereUpdater::Tick at all. "
			     "Either the pulse had another cause or this map drives the fog from somewhere else - "
			     "either way that is a finding, not a pass."),
			GFPMFogFramesHeld);
	}
}

static FAutoConsoleCommandWithOutputDevice GFPMIndoorFogReportCmd(
	TEXT("FPM.Fog.Report"),
	TEXT("Indoor fog gate: under-roof state, the game's own StartDistance, our target, and whether the "
	     "game re-drives the property at all."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMIndoorFog::ReportNow();
	}));
