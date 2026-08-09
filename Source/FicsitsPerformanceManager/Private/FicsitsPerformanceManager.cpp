// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "FicsitsPerformanceManager.h"

#include "Core/FPMBoxCache.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMHitchMeter.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMOverlay.h"
#include "Core/FPMSaveSettingsInterceptor.h"
#include "Core/FPMUserSettingMap.h"
#include "Fixes/Interop/FPMHologramNetGuard.h"
#include "Fixes/Interop/FPMHudHookGuard.h"
#include "Fixes/Interop/FPMInventoryInitGuard.h"
#include "Fixes/Interop/FPMNoOwnerRpcGate.h"
#include "Fixes/Interop/FPMNavMeshCeiling.h"
#include "Fixes/Interop/FPMRailConnectionGuard.h"
#include "Fixes/Interop/FPMRainOcclusionFix.h"
#include "Fixes/Interop/FPMSchematicProbe.h"
#include "Fixes/Interop/FPMStaticBaseFix.h"
#include "Fixes/Interop/FPMTexturePoolGuard.h"
#include "Fixes/Interop/FPMZiplineVolume.h"
#include "Fixes/Vanilla/FPMCloneSensor.h"
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

	// MEASUREMENT ONLY, AND IT INSTALLS NO HOOKS - it subscribes to two engine delegates and a ticker, so it
	// will not appear in the hook ledger below. Armed here because Ant's hitches (2026-08-09) had no
	// instrument at all: the engine's own hitch detector is compiled out of this build, which the header
	// proves from the build's preprocessor definitions rather than from reputation. It arms on the dedicated
	// server too - the 560 ms save stall is a server-side hitch and a client-only readout could never see it.
	FPMFixes::Arm(FFPMHitchMeter::Get());

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
