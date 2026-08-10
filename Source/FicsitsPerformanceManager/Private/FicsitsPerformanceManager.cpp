// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "FicsitsPerformanceManager.h"

#include "Core/FPMBoxCache.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMGCMeter.h"
#include "Core/FPMStallSampler.h"
#include "Core/FPMHitchMeter.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMOverlay.h"
#include "Core/FPMSaveSettingsInterceptor.h"
#include "Core/FPMUserSettingMap.h"
#include "Fixes/Interop/FPMDistanceFieldAudit.h"
#include "Fixes/Interop/FPMHologramNetGuard.h"
#include "Fixes/Interop/FPMHudHookGuard.h"
#include "Fixes/Interop/FPMInventoryInitGuard.h"
#include "Fixes/Interop/FPMNoOwnerRpcGate.h"
#include "Fixes/Interop/FPMNavMeshCeiling.h"
#include "Fixes/Interop/FPMRailConnectionGuard.h"
#include "Fixes/Interop/FPMRainOcclusionFix.h"
#include "Fixes/Interop/FPMSchematicNullGuard.h"
#include "Fixes/Interop/FPMSchematicProbe.h"
#include "Fixes/Interop/FPMStaticBaseFix.h"
#include "Fixes/Interop/FPMTexturePoolGuard.h"
#include "Fixes/Interop/FPMZiplineVolume.h"
#include "Core/FPMCrashStamp.h"
#include "Fixes/Interop/FPMWeatherIndoorGate.h"
#include "Fixes/Interop/FPMWwiseServerGate.h"
#include "Fixes/Vanilla/FPMCloneSensor.h"
#include "Fixes/Vanilla/FPMPowerWarningProbe.h"
#include "Fixes/Vanilla/FPMWireNullGuard.h"
#include "Streaming/FPMAssetResidency.h"

#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FFicsitsPerformanceManagerModule"

DEFINE_LOG_CATEGORY(LogFicsitsPerformanceManager);

void FFicsitsPerformanceManagerModule::StartupModule()
{
	// The version is read out of the LOADED plugin descriptor rather than baked in as a literal at
	// compile time. Those two can disagree: a package that silently no-ops ships yesterday's binary
	// beside today's .uplugin, and this project has burned boots believing the source version. What
	// this line prints is what is actually running, which is the only version worth attributing a
	// measurement to.
	FString VersionName = TEXT("<no descriptor>");
	if (const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("FicsitsPerformanceManager")))
	{
		VersionName = Self->GetDescriptor().VersionName;
	}

	constexpr const TCHAR* BuildKind = WITH_EDITOR ? TEXT("editor") : TEXT("game");

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] runtime module loaded - version %s, %s build"), *VersionName, BuildKind);

	/*
	 * THE WRITER'S SELF-TEST RUNS BEFORE ANY FIX ARMS, and it runs EVERY boot rather than once in a test
	 * branch. What it checks is an ENGINE behaviour — that a tagged 0x07 write can be taken back with
	 * Unset — and a game update can change that under us. A release path that silently stopped working
	 * would look perfect right up until an uninstall left residue behind, which is the one failure this
	 * mod's whole posture exists to prevent.
	 */
	FPMCVarWriter::Get().SelfTest();

	// Remove the cache older builds left OUTSIDE the plugin. Found by audit 2026-08-09: 120,681 bytes
	// under the game's Saved directory that survived uninstall while the sentinel reported nothing.
	FPMBoxCache::CleanUpLegacyResidue();

	// ARMING. There is no `if (!WITH_EDITOR)` here on purpose: the editor gate lives inside
	// FPMHookLedger::Install, so a fix's Arm() still COMPILES and RUNS in the editor while installing
	// nothing. hooking.adoc:124 warns that `#if` around the SUBSCRIBE calls hides errors until a
	// shipping build - putting the single gate at the one place every hook passes through gets the
	// protection without the blind spot.
	// Arm order IS install order, and SML calls hooks in the order they registered
	// (hooking.adoc:43). Nothing here contends for a target today, but the order is recorded by the
	// ledger so a future collision is a readable log line rather than a mystery.
	FPMFixes::Arm(FFPMStaticBaseFix::Get());
	FPMFixes::Arm(FFPMNoOwnerRpcGate::Get());
	FPMFixes::Arm(FFPMCloneSensor::Get());
	FPMFixes::Arm(FFPMRainOcclusionFix::Get());

	// The two join-crash repairs, carried from the old mod on 2026-08-08 and REBUILT rather than copied:
	// both used to prevent their crash by cancelling the work, and both cost something real for it — an
	// unrendered build preview (Ant saw it) and a possibly-destroyed item stack. They now supply the data
	// that was missing and let vanilla run.
	FPMFixes::Arm(FFPMInventoryInitGuard::Get());
	FPMFixes::Arm(FFPMHologramNetGuard::Get());

	// P3.2. This one DOES override: an unwired rail connection gets nullptr instead of vanilla's assert.
	// Measured on the old mod across 11 server sessions: 1,900-2,550 averted asserts per start, 23,450
	// total. A properly-wired connection is forwarded untouched.
	FPMFixes::Arm(FFPMRailConnectionGuard::Get());

	// P3.4. Installs no hook; it acts at world load and READS BACK, because its predecessor logged a
	// raise the engine ignored for twenty-two seconds.
	FPMFixes::Arm(FFPMNavMeshCeiling::Get());

	// Installs no hook either - it sweeps at world load. Guards the autosave path that took the
	// dedicated server down on 2026-08-09 (null UClass in FFastSaveReferenceCollector, from a null entry
	// in a power-wire array). Armed on BOTH sides: the client log carries the same 2,549 unresolvable
	// level references the server does, so the same invalid state exists there.
	FPMFixes::Arm(FFPMWireNullGuard::Get());

	/*
	 * P3.9's power-warning item, and it is the DIAGNOSTIC half only. Ant sees a "Fuse Blown" popup on
	 * every login in single player and on the server, and two research passes found vanilla's graph
	 * never reads the payload it is handed — so a false positive is plausible. Plausible is not
	 * measured, and LAW 13 says do not suppress a warning until we know it is lying.
	 *
	 * It installs NO HOOK on purpose. The emitter is a BlueprintNativeEvent whose native body is empty
	 * (FGCircuitSubsystem.cpp:47) because BP_CircuitSubsystem implements the popup in Blueprint, so a
	 * SUBSCRIBE_METHOD there would never fire and its silence would read as "no bug". It reads circuit
	 * STATE instead, over a window, because mIsFuseTriggered is replicated and arrives after a join.
	 */
	FPMFixes::Arm(FFPMPowerWarningProbe::Get());

	/*
	 * ONE SUSPECTED CAUSE UNDER THREE OF ANT'S SYMPTOMS: rain through built walls, light through terrain
	 * on low settings, and the parked distance-field shadow pop-in. AbstractInstance switches distance
	 * fields OFF on instanced meshes during lazy load and re-enables them in a ONE-SHOT pass when the
	 * queues drain (AbstractInstanceManager.cpp:305, :482-498). Anything that missed that pass is
	 * invisible to every distance-field consumer in the renderer, permanently.
	 *
	 * Reading more vanilla source cannot settle whether it happens in HER world -- it depends on
	 * streaming order in a specific save. So this counts. Audit only by default: the repair adds renderer
	 * work, and Ant's constraint is "keep the performance good".
	 */
	FPMFixes::Arm(FFPMDistanceFieldAudit::Get());

	/*
	 * Tier 2 of the particle work, and it is the CHEAP half by design. Ant: "I want the dirt particle
	 * stuffs to be included in the fog stuff so stuff stops going through the walls when inside."
	 *
	 * It is NOT collision and does not claim to be. NS_Wind, NS_Forest_Field_Wind and
	 * NS_Forest_Red_Wind ship with NO collision module at all (counted from the export), and no runtime
	 * call can add one that was never compiled into the emitter script. This scales their intensity down
	 * while the shared enclosure check says the player is in a SEALED ROOM -- walls, not just a roof.
	 *
	 * ⚠ Arming it is what STARTS the enclosure sampler. Nothing traces until something asks.
	 */
	FPMFixes::Arm(FFPMWeatherIndoorGate::Get());

	// Re-port of a fix the rewrite orphaned (FPM1 RegisterWwiseServerAudioGate). Installs a hook ONLY
	// on a dedicated server, where UAkGameplayStatics::StopActor cannot reach an audio device and so
	// only writes a warning — 681 of them in the 2026-08-09 server session. A client installs nothing.
	FPMFixes::Arm(FFPMWwiseServerGate::Get());

	// P3.5. CLIENT ONLY by contract, not by a hand-rolled early return - a server has no player HUD and
	// the old mod's server failed to boot carrying an earlier version of this.
	FPMFixes::Arm(FFPMHudHookGuard::Get());

	// P3.9. Client-only by contract. Default 1.0 writes nothing, so vanilla is bit-identical until Ant
	// moves FPM.Zipline.Volume.
	FPMFixes::Arm(FFPMZiplineVolume::Get());

	/*
	 * The card-sized streaming pool. Measured on Ant's machine 2026-08-09 BEFORE this shipped: vanilla's
	 * flat 1000 MB pool cost 57 FPS vs 92, and a 1% low of 46 vs 69, on a 16303 MB card - the GPU was
	 * idling at 83% waiting for textures that were not resident. It writes nothing for the first 45
	 * seconds, and nothing at all on cards below the tier.
	 */
	FPMFixes::Arm(FFPMTexturePoolGuard::Get());

	// LOG-ONLY. Ant, on the old mod's forced-TRUE milestone override: "maybe carry it with just
	// diagnostics and we'll see what happens?" and "diagnostics are good either way so we KNOW what
	// breaks and why". It overrides nothing - the crash dumps show this is a VANILLA crash (one has
	// neither FPM nor KPrivateCodeLib on the stack), so there is nothing here for us to guard yet.
	FPMFixes::Arm(FFPMSchematicProbe::Get());

	/*
	 * ★ P3.10(a), AND IT MUST ARM AFTER THE PROBE ABOVE. NOT A STYLE PREFERENCE — A CORRECTNESS ONE.
	 *
	 * SML chains handlers on a target in registration order, and `TCallScope::Override` sets
	 * `bForwardCall = false`, which stops the chain: every handler registered LATER is skipped
	 * (NativeHookManager.h:216-228). Both of these sit on UFGSchematic::CanGiveAccessToSchematic. Arm
	 * the guard first and it would silence the probe on precisely the calls worth observing — the
	 * refusals. The probe never overrides, so probe-then-guard preserves both instruments.
	 *
	 * ⚠ IT IS NOT THE FIX THE DESIGN SPECIFIES, AND THE HEADER SAYS WHY AT LENGTH. The design's
	 * argument-only check shipped as FPM1 0.58.52 and fourteen crashes of this class followed it. This
	 * refuses on the condition all nineteen dumps actually share: relevant events declared with no event
	 * subsystem to evaluate them against.
	 */
	FPMFixes::Arm(FFPMSchematicNullGuard::Get());

	// MEASUREMENT ONLY, AND IT INSTALLS NO HOOKS - it subscribes to two engine delegates and a ticker, so it
	// will not appear in the hook ledger below. Armed here because Ant's hitches (2026-08-09) had no
	// instrument at all: the engine's own hitch detector is compiled out of this build, which the header
	// proves from the build's preprocessor definitions rather than from reputation. It arms on the dedicated
	// server too - the 560 ms save stall is a server-side hitch and a client-only readout could never see it.
	FPMFixes::Arm(FFPMHitchMeter::Get());

	/*
	 * The OTHER hitch class, and it is genuinely separate from the async-load one the residency fix took.
	 * Measured 2026-08-02 on Ant's save: 27 GC pauses in ~22 min, mean 27.2 ms, worst 148.6 ms, every one
	 * stop-the-world.
	 *
	 * L1 of a design that was written then orphaned by the rewrite. Measurement ships before any lever
	 * because the only pacing lever available stretches TIMER passes and leaves FORCED ones untouched --
	 * so the split this meter produces decides whether that lever is worth writing at all. It steers
	 * nothing: no quality lever shortens a mark that scales with the live object graph.
	 */
	FPMFixes::Arm(FFPMGCMeter::Get());

	/*
	 * ★ AND THE ONE THAT PICKS UP WHERE COUNTING STOPS. Armed 2026-08-10, straight off the 0.8.4 boot.
	 *
	 * That boot narrowed the hitches as far as buckets can go: 54 of 55 GAME-THREAD BOUND with the game
	 * thread busy ~99.9% of the span, 83-100% matching none of the six cause buckets, and the render
	 * thread idle at 11 ms while the game thread burned 400. The meter proved WHERE the time goes and
	 * exhausted its ability to say WHAT is spending it.
	 *
	 * Only a sample of the thread's own callstack answers that. This resolves to MODULE rather than
	 * function — a retail install ships no PDBs — which with 53 mods on the server and 124 in her client
	 * profile is the question anyway: WHICH ONE.
	 *
	 * It suspends the game thread to read it, so it is capped, gapped, and reports its own budget usage.
	 */
	FPMFixes::Arm(FFPMStallSampler::Get());

	// And the thing the meter is pointed AT. Root-caused 2026-08-02, recovered unbuilt by the 2026-08-09
	// scratchpad audit: vanilla's own player-list widget blocking-loads a platform icon nobody holds a hard
	// reference to, every time it binds. It installs no hook either - it pins four vanilla textures so
	// vanilla's LoadSynchronous finds them already resident and skips its blocking branch.
	FPMFixes::Arm(FFPMAssetResidency::Get());

	// Always printed, even when empty - an empty inventory is itself a finding.
	FPMHookLedger::LogInventory();

	/*
	 * Binds the user-setting map's automatic read to post-engine-init, so every boot reports which cvars
	 * the game's own settings save would capture WITHOUT anyone typing a console command. Measured
	 * 2026-08-09: a console command cannot be delivered from outside this game - UE strips -ExecCmds in
	 * Shipping, SML reimplements it, and Steam then replaces the command line with its own options. So a
	 * diagnostic that must be typed costs one of Ant's boots; this one does not.
	 */
	FPMUserSettingMap::Init();

	// Armed AFTER the map, because its arm-time self-test reports which source the map is answering
	// from, and BEFORE anything can hold a cvar — the guard refuses to arm if a hold already exists.
	FPMFixes::Arm(FFPMSaveSettingsInterceptor::Get());

	// The debug feed, on by default while the mod is pre-release. Ant: "i want UI to show when and what
	// the rain fix thing is doing so i can see that its working." It attaches as soon as a viewport
	// exists, which is why it can report during a loading screen at all.
	if (!IsRunningDedicatedServer())
	{
		// Ant asked for this three times. It is installed BEFORE the overlay is shown, so the very first
		// thing she can do with it is turn it off.
		FPMOverlay::Get().InstallHotkey();
		FPMOverlay::Get().SetVisible(true);
		FPMOverlay::Post(TEXT("startup"), FString::Printf(TEXT("FPM %s loaded, %d hook(s) armed"),
			*VersionName, FPMHookLedger::Records().Num()));
	}

	/*
	 * ★ LAST, DELIBERATELY. The crash stamp records the ARMED-FIX ROSTER, so it has to run after every
	 * Arm() above — stamping earlier would write an empty roster and look like it worked.
	 *
	 * P1.1's remaining deliverable (§7.2, the R4-M2a slot). It registers version, side, roster and hook
	 * counts into the crash context NOW, so a dump identifies FPM without needing FactoryGame.log —
	 * which after a server crash is usually already rotated away. Its predecessor wrote at crash time
	 * and went 0-for-3 on access violations in Shipping, because OnHandleSystemError never fires for
	 * one; this writes ahead of the crash instead.
	 */
	FPMCrashStamp::Register(VersionName);
}

void FFicsitsPerformanceManagerModule::ShutdownModule()
{
	// Release before anything else tears down: the OFF switch must run while the console manager is
	// still alive. Nothing was captured, so this cannot restore a wrong value -- it only stops holding.
	FPMCVarWriter::Get().ReleaseAll(TEXT("module shutdown"));

	FPMOverlay::Get().Shutdown();
	FPMFixes::DisarmAll();
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] runtime module unloading"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFicsitsPerformanceManagerModule, FicsitsPerformanceManager)
